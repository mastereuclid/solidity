/*(
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Base class to perform data flow analysis during AST walks.
 * Tracks assignments and is used as base class for both Rematerialiser and
 * Common Subexpression Eliminator.
 */

#include <libyul/optimiser/DataFlowAnalyzer.h>

#include <libyul/optimiser/NameCollector.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/Exceptions.h>
#include <libyul/AsmData.h>
#include <libyul/backends/evm/EVMDialect.h>

#include <libdevcore/CommonData.h>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

using namespace std;
using namespace dev;
using namespace yul;


// For elementary statements, we check if it is an SSTORE(x, y)
//   If yes, visit the statement. Then record that fact and clear all storage slots t
//     where we cannot prove x != t or y == m_storage[t] using the current values of the variables x and t.
//   Otherwise, determine if the statement invalidates storage. If yes, clear all knowledge
//     about storage before visiting the statement. Then visit the statement.
//
// For forward-joining control flow, storage information from the branches is combined.
// If the keys or values are different or non-existent in one branch, the key is deleted.

void DataFlowAnalyzer::operator()(ExpressionStatement& _statement)
{
	if (boost::optional<pair<YulString, YulString>> vars = isSimpleSStore(_statement))
	{
		ASTModifier::operator()(_statement);
		m_storage.set(vars->first, vars->second);
		// TODO could be made more efficient
		set<YulString> keysToErase;
		for (auto const& item: m_storage.values)
			if (!(
				m_knowledgeBase.knownToBeDifferent(vars->first, item.first) ||
				m_knowledgeBase.knownToBeEqual(vars->second, item.second)
			))
				keysToErase.insert(item.first);
		for (YulString const& key: keysToErase)
			m_storage.eraseKey(key);
	}
	else
	{
		clearStorageKnowledgeIfInvalidated(_statement.expression);
		ASTModifier::operator()(_statement);
	}
}

void DataFlowAnalyzer::operator()(Assignment& _assignment)
{
	set<YulString> names;
	for (auto const& var: _assignment.variableNames)
		names.emplace(var.name);
	assertThrow(_assignment.value, OptimizerException, "");
	clearStorageKnowledgeIfInvalidated(*_assignment.value);
	visit(*_assignment.value);
	handleAssignment(names, _assignment.value.get());
}

void DataFlowAnalyzer::operator()(VariableDeclaration& _varDecl)
{
	set<YulString> names;
	for (auto const& var: _varDecl.variables)
		names.emplace(var.name);
	m_variableScopes.back().variables += names;

	if (_varDecl.value)
	{
		clearStorageKnowledgeIfInvalidated(*_varDecl.value);
		visit(*_varDecl.value);
	}

	handleAssignment(names, _varDecl.value.get());
}

void DataFlowAnalyzer::operator()(If& _if)
{
	clearStorageKnowledgeIfInvalidated(*_if.condition);
	InvertibleMap<YulString, YulString> storage = m_storage;

	ASTModifier::operator()(_if);

	joinStorageKnowledge(storage);

	Assignments assignments;
	assignments(_if.body);
	clearValues(assignments.names());
}

void DataFlowAnalyzer::operator()(Switch& _switch)
{
	clearStorageKnowledgeIfInvalidated(*_switch.expression);
	visit(*_switch.expression);
	set<YulString> assignedVariables;
	for (auto& _case: _switch.cases)
	{
		InvertibleMap<YulString, YulString> storage = m_storage;
		(*this)(_case.body);
		joinStorageKnowledge(storage);

		Assignments assignments;
		assignments(_case.body);
		assignedVariables += assignments.names();
		// This is a little too destructive, we could retain the old values.
		clearValues(assignments.names());
		clearStorageKnowledgeIfInvalidated(_case.body);
	}
	for (auto& _case: _switch.cases)
		clearStorageKnowledgeIfInvalidated(_case.body);
	clearValues(assignedVariables);
}

void DataFlowAnalyzer::operator()(FunctionDefinition& _fun)
{
	// Save all information. We might rather reinstantiate this class,
	// but this could be difficult if it is subclassed.
	map<YulString, Expression const*> value;
	InvertibleRelation<YulString> references;
	InvertibleMap<YulString, YulString> storage;
	m_value.swap(value);
	swap(m_references, references);
	swap(m_storage, storage);
	pushScope(true);

	for (auto const& parameter: _fun.parameters)
		m_variableScopes.back().variables.emplace(parameter.name);
	for (auto const& var: _fun.returnVariables)
	{
		m_variableScopes.back().variables.emplace(var.name);
		handleAssignment({var.name}, nullptr);
	}
	ASTModifier::operator()(_fun);

	popScope();
	m_value.swap(value);
	swap(m_references, references);
	swap(m_storage, storage);
}

void DataFlowAnalyzer::operator()(ForLoop& _for)
{
	// If the pre block was not empty,
	// we would have to deal with more complicated scoping rules.
	assertThrow(_for.pre.statements.empty(), OptimizerException, "");

	// TODO break/continue could be tricky for storage.
	// We almost always clear here.

	AssignmentsSinceContinue assignmentsSinceCont;
	assignmentsSinceCont(_for.body);

	Assignments assignments;
	assignments(_for.body);
	assignments(_for.post);
	clearValues(assignments.names());
	clearStorageKnowledgeIfInvalidated(*_for.condition);
	clearStorageKnowledgeIfInvalidated(_for.post);
	clearStorageKnowledgeIfInvalidated(_for.body);

	visit(*_for.condition);
	(*this)(_for.body);
	clearValues(assignmentsSinceCont.names());
	clearStorageKnowledgeIfInvalidated(_for.body);
	(*this)(_for.post);
	clearValues(assignments.names());
	clearStorageKnowledgeIfInvalidated(*_for.condition);
	clearStorageKnowledgeIfInvalidated(_for.post);
	clearStorageKnowledgeIfInvalidated(_for.body);
}

void DataFlowAnalyzer::operator()(Block& _block)
{
	size_t numScopes = m_variableScopes.size();
	pushScope(false);
	ASTModifier::operator()(_block);
	popScope();
	assertThrow(numScopes == m_variableScopes.size(), OptimizerException, "");
}

void DataFlowAnalyzer::handleAssignment(set<YulString> const& _variables, Expression* _value)
{
	clearValues(_variables);

	MovableChecker movableChecker{m_dialect};
	if (_value)
		movableChecker.visit(*_value);
	else
		for (auto const& var: _variables)
			m_value[var] = &m_zero;

	if (_value && _variables.size() == 1)
	{
		YulString name = *_variables.begin();
		// Expression has to be movable and cannot contain a reference
		// to the variable that will be assigned to.
		if (movableChecker.movable() && !movableChecker.referencedVariables().count(name))
			m_value[name] = _value;
	}

	auto const& referencedVariables = movableChecker.referencedVariables();
	for (auto const& name: _variables)
	{
		m_references.set(name, referencedVariables);
		// assignment to slot denoted by "name"
		m_storage.eraseKey(name);
		// assignment to slot contents denoted by "name"
		m_storage.eraseValue(name);
	}
}

void DataFlowAnalyzer::pushScope(bool _functionScope)
{
	m_variableScopes.emplace_back(_functionScope);
}

void DataFlowAnalyzer::popScope()
{
	clearValues(std::move(m_variableScopes.back().variables));
	m_variableScopes.pop_back();
}

void DataFlowAnalyzer::clearValues(set<YulString> _variables)
{
	// All variables that reference variables to be cleared also have to be
	// cleared, but not recursively, since only the value of the original
	// variables changes. Example:
	// let a := 1
	// let b := a
	// let c := b
	// let a := 2
	// add(b, c)
	// In the last line, we can replace c by b, but not b by a.
	//
	// This cannot be easily tested since the substitutions will be done
	// one by one on the fly, and the last line will just be add(1, 1)

	// First clear storage knowledge, because we do not have to clear
	// storage knowledge of variables whose expression has changed,
	// since the value is still unchanged.
	for (auto const& name: _variables)
	{
		// clear slot denoted by "name"
		m_storage.eraseKey(name);
		// clear slot contents denoted by "name"
		m_storage.eraseValue(name);
	}

	// Also clear variables that reference variables to be cleared.
	for (auto const& name: _variables)
		for (auto const& ref: m_references.backward[name])
			_variables.emplace(ref);

	// Clear the value and update the reference relation.
	for (auto const& name: _variables)
		m_value.erase(name);
	for (auto const& name: _variables)
		m_references.eraseKey(name);
}

void DataFlowAnalyzer::clearStorageKnowledgeIfInvalidated(Block const& _block)
{
	if (InvalidationChecker::invalidatesStorage(m_dialect, _block))
		m_storage.clear();
}

void DataFlowAnalyzer::clearStorageKnowledgeIfInvalidated(Expression const& _expr)
{
	if (InvalidationChecker::invalidatesStorage(m_dialect, _expr))
		m_storage.clear();
}

void DataFlowAnalyzer::joinStorageKnowledge(InvertibleMap<YulString, YulString> const& _other)
{
	set<YulString> keysToErase;
	for (auto const& item: m_storage.values)
	{
		auto it = _other.values.find(item.first);
		if (it == _other.values.end() || it->second != item.second)
			keysToErase.insert(item.first);
	}
	for (auto const& key: keysToErase)
		m_storage.eraseKey(key);
}

bool DataFlowAnalyzer::inScope(YulString _variableName) const
{
	for (auto const& scope: m_variableScopes | boost::adaptors::reversed)
	{
		if (scope.variables.count(_variableName))
			return true;
		if (scope.isFunction)
			return false;
	}
	return false;
}

boost::optional<pair<YulString, YulString>> DataFlowAnalyzer::isSimpleSStore(
	ExpressionStatement const& _statement
) const
{
	if (_statement.expression.type() == typeid(FunctionCall))
	{
		FunctionCall const& funCall = boost::get<FunctionCall>(_statement.expression);
		if (EVMDialect const* dialect = dynamic_cast<EVMDialect const*>(&m_dialect))
			if (auto const* builtin = dialect->builtin(funCall.functionName.name))
				if (builtin->instruction == dev::eth::Instruction::SSTORE)
					if (
						funCall.arguments.at(0).type() == typeid(Identifier) &&
						funCall.arguments.at(1).type() == typeid(Identifier)
					)
					{
						YulString key = boost::get<Identifier>(funCall.arguments.at(0)).name;
						YulString value = boost::get<Identifier>(funCall.arguments.at(1)).name;
						return make_pair(key, value);
					}
	}
	return {};
}

