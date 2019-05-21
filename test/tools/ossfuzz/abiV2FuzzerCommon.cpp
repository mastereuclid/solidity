#include <test/tools/ossfuzz/abiV2FuzzerCommon.h>

using namespace dev::test::abiv2fuzzer;

SolidityCompilationFramework::SolidityCompilationFramework(langutil::EVMVersion _evmVersion)
{
	m_evmVersion = _evmVersion;
}
