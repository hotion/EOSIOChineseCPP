
//此源码被清华学神尹成大魔王专业翻译分析并修改
//尹成QQ77025077
//尹成微信18510341407
//尹成所在QQ群721929980
//尹成邮箱 yinc13@mails.tsinghua.edu.cn
//尹成毕业于清华大学,微软区块链领域全球最有价值专家
//https://mvp.microsoft.com/zh-cn/PublicProfile/4033620
#include "Inline/BasicTypes.h"
#include "Inline/Timing.h"
#include "Platform/Platform.h"
#include "WAST/WAST.h"
#include "Runtime/Runtime.h"
#include "Runtime/Linker.h"
#include "Runtime/Intrinsics.h"
#include "Emscripten/Emscripten.h"
#include "IR/Module.h"
#include "IR/Operators.h"
#include "IR/Validate.h"

#include "CLI.h"

#include <map>

using namespace IR;
using namespace Runtime;

void showHelp()
{
	std::cerr << "Usage: wavm [switches] [programfile] [--] [arguments]" << std::endl;
	std::cerr << "  in.wast|in.wasm\t\tSpecify program file (.wast/.wasm)" << std::endl;
	std::cerr << "  -f|--function name\t\tSpecify function name to run in module rather than main" << std::endl;
	std::cerr << "  -c|--check\t\t\tExit after checking that the program is valid" << std::endl;
	std::cerr << "  -d|--debug\t\t\tWrite additional debug information to stdout" << std::endl;
	std::cerr << "  --\t\t\t\tStop parsing arguments" << std::endl;
}

struct RootResolver : Resolver
{
	std::map<std::string,Resolver*> moduleNameToResolverMap;

	bool resolve(const std::string& moduleName,const std::string& exportName,ObjectType type,ObjectInstance*& outObject) override
	{
//首先尝试解决一个固有问题。
		if(IntrinsicResolver::singleton.resolve(moduleName,exportName,type,outObject)) { return true; }

//然后查找命名模块。
		auto namedResolverIt = moduleNameToResolverMap.find(moduleName);
		if(namedResolverIt != moduleNameToResolverMap.end())
		{
			return namedResolverIt->second->resolve(moduleName,exportName,type,outObject);
		}

//最后，将缺失的函数导入存根。
		if(type.kind == ObjectKind::function)
		{
//生成一个函数体，该函数体在调用时只使用不可访问的op进行故障处理。
			Serialization::ArrayOutputStream codeStream;
			OperatorEncoderStream encoder(codeStream);
			encoder.unreachable();
			encoder.end();

//为存根函数生成一个模块。
			Module stubModule;
			DisassemblyNames stubModuleNames;
			stubModule.types.push_back(asFunctionType(type));
			stubModule.functions.defs.push_back({{0},{},std::move(codeStream.getBytes()),{}});
			stubModule.exports.push_back({"importStub",ObjectKind::function,0});
			stubModuleNames.functions.push_back({std::string(moduleName) + "." + exportName,{}});
			IR::setDisassemblyNames(stubModule,stubModuleNames);
			IR::validateDefinitions(stubModule);

//实例化模块并返回存根函数实例。
			auto stubModuleInstance = instantiateModule(stubModule,{});
			outObject = getInstanceExport(stubModuleInstance,"importStub");
			Log::printf(Log::Category::error,"Generated stub for missing function import %s.%s : %s\n",moduleName.c_str(),exportName.c_str(),asString(type).c_str());
			return true;
		}
		else if(type.kind == ObjectKind::memory)
		{
			outObject = asObject(Runtime::createMemory(asMemoryType(type)));
			Log::printf(Log::Category::error,"Generated stub for missing memory import %s.%s : %s\n",moduleName.c_str(),exportName.c_str(),asString(type).c_str());
			return true;
		}
		else if(type.kind == ObjectKind::table)
		{
			outObject = asObject(Runtime::createTable(asTableType(type)));
			Log::printf(Log::Category::error,"Generated stub for missing table import %s.%s : %s\n",moduleName.c_str(),exportName.c_str(),asString(type).c_str());
			return true;
		}
		else if(type.kind == ObjectKind::global)
		{
			outObject = asObject(Runtime::createGlobal(asGlobalType(type),Runtime::Value(asGlobalType(type).valueType,Runtime::UntaggedValue())));
			Log::printf(Log::Category::error,"Generated stub for missing global import %s.%s : %s\n",moduleName.c_str(),exportName.c_str(),asString(type).c_str());
			return true;
		}

		return false;
	}
};

int mainBody(const char* filename,const char* functionName,bool onlyCheck,char** args)
{
	Module module;
	if(filename)
	{
		if(!loadModule(filename,module)) { return EXIT_FAILURE; }
	}
	else
	{
		showHelp();
		return EXIT_FAILURE;
	}

	if(onlyCheck) { return EXIT_SUCCESS; }

//链接并实例化模块。
	RootResolver rootResolver;
	LinkResult linkResult = linkModule(module,rootResolver);
	if(!linkResult.success)
	{
		std::cerr << "Failed to link module:" << std::endl;
		for(auto& missingImport : linkResult.missingImports)
		{
			std::cerr << "Missing import: module=\"" << missingImport.moduleName
				<< "\" export=\"" << missingImport.exportName
				<< "\" type=\"" << asString(missingImport.type) << "\"" << std::endl;
		}
		return EXIT_FAILURE;
	}
	ModuleInstance* moduleInstance = instantiateModule(module,std::move(linkResult.resolvedImports));
	if(!moduleInstance) { return EXIT_FAILURE; }
	Emscripten::initInstance(module,moduleInstance);

//查找要调用的函数export。
	FunctionInstance* functionInstance;
	if(!functionName)
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"main"));
		if(!functionInstance) { functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,"_main")); }
		if(!functionInstance)
		{
			std::cerr << "Module does not export main function" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,functionName));
		if(!functionInstance)
		{
			std::cerr << "Module does not export '" << functionName << "'" << std::endl;
			return EXIT_FAILURE;
		}
	}
	const FunctionType* functionType = getFunctionType(functionInstance);

//设置调用的参数。
	std::vector<Value> invokeArgs;
	if(!functionName)
	{
		if(functionType->parameters.size() == 2)
		{
			MemoryInstance* defaultMemory = Runtime::getDefaultMemory(moduleInstance);
			if(!defaultMemory)
			{
				std::cerr << "Module does not declare a default memory object to put arguments in." << std::endl;
				return EXIT_FAILURE;
			}

			std::vector<const char*> argStrings;
			argStrings.push_back(filename);
			while(*args) { argStrings.push_back(*args++); };

			Emscripten::injectCommandArgs(argStrings,invokeArgs);
		}
		else if(functionType->parameters.size() > 0)
		{
			std::cerr << "WebAssembly function requires " << functionType->parameters.size() << " argument(s), but only 0 or 2 can be passed!" << std::endl;
			return EXIT_FAILURE;
		}
	}
	else
	{
		for(U32 i = 0; args[i]; ++i)
		{
			Value value;
			switch(functionType->parameters[i])
			{
			case ValueType::i32: value = (U32)atoi(args[i]); break;
			case ValueType::i64: value = (U64)atol(args[i]); break;
			case ValueType::f32: value = (F32)atof(args[i]); break;
			case ValueType::f64: value = atof(args[i]); break;
			default: Errors::unreachable();
			}
			invokeArgs.push_back(value);
		}
	}

//调用函数。
	Timing::Timer executionTimer;
	auto functionResult = invokeFunction(functionInstance,invokeArgs);
	Timing::logTimer("Invoked function",executionTimer);

	if(functionName)
	{
		Log::printf(Log::Category::debug,"%s returned: %s\n",functionName,asString(functionResult).c_str());
		return EXIT_SUCCESS;
	}
	else if(functionResult.type == ResultType::i32) { return functionResult.i32; }
	else { return EXIT_SUCCESS; }
}

int commandMain(int argc,char** argv)
{
	const char* filename = nullptr;
	const char* functionName = nullptr;

	bool onlyCheck = false;
	auto args = argv;
	while(*++args)
	{
		if(!strcmp(*args, "--function") || !strcmp(*args, "-f"))
		{
			if(!*++args) { showHelp(); return EXIT_FAILURE; }
			functionName = *args;
		}
		else if(!strcmp(*args, "--check") || !strcmp(*args, "-c"))
		{
			onlyCheck = true;
		}
		else if(!strcmp(*args, "--debug") || !strcmp(*args, "-d"))
		{
			Log::setCategoryEnabled(Log::Category::debug,true);
		}
		else if(!strcmp(*args, "--"))
		{
			++args;
			break;
		}
		else if(!strcmp(*args, "--help") || !strcmp(*args, "-h"))
		{
			showHelp();
			return EXIT_SUCCESS;
		}
		else if(!filename)
		{
			filename = *args;
		}
		else { break; }
	}

	Runtime::init();

	int returnCode = EXIT_FAILURE;
	#ifdef __AFL_LOOP
	while(__AFL_LOOP(2000))
	#endif
	{
		returnCode = mainBody(filename,functionName,onlyCheck,args);
		Runtime::freeUnreferencedObjects({});
	}
	return returnCode;
}
