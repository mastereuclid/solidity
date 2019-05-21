#pragma once

#include <libsolidity/interface/CompilerStack.h>
#include <libyul/AssemblyStack.h>
#include <liblangutil/Exceptions.h>
#include <liblangutil/SourceReferenceFormatter.h>
#include <libdevcore/Keccak256.h>

namespace dev
{
namespace test
{
namespace abiv2fuzzer
{
class SolidityCompilationFramework
{
public:
	explicit SolidityCompilationFramework(langutil::EVMVersion _evmVersion = {});

	Json::Value getMethodIdentifiers()
	{
		return m_compiler.methodIdentifiers(m_compiler.lastContractName());
	}

	dev::bytes compileContract(std::string const& _sourceCode)
	{
		// Silence compiler version warning
		std::string sourceCode = "pragma solidity >=0.0;\n";
		sourceCode += "pragma experimental ABIEncoderV2;\n";
		sourceCode += _sourceCode;
		m_compiler.setSources({{"", sourceCode}});
		m_compiler.setEVMVersion(m_evmVersion);
		m_compiler.setOptimiserSettings(m_optimiserSettings);
		if (!m_compiler.compile())
		{
			langutil::SourceReferenceFormatter formatter(std::cerr);

			for (auto const& error: m_compiler.errors())
				formatter.printExceptionInformation(
						*error,
						formatter.formatErrorInformation(*error)
				);
			std::cerr << "Compiling contract failed" << std::endl;
		}
		dev::eth::LinkerObject obj = m_compiler.runtimeObject(m_compiler.lastContractName());
		return obj.bytecode;
	}

protected:
	dev::solidity::CompilerStack m_compiler;
	langutil::EVMVersion m_evmVersion;
	dev::solidity::OptimiserSettings m_optimiserSettings = dev::solidity::OptimiserSettings::full();
};
}
}
}
