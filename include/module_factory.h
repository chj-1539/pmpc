#ifndef MODULE_FACTORY_H
#define MODULE_FACTORY_H

//=============================================================================
// module_factory.h — 模块自动注册工厂
//
// 用法：
//   在模块的 .cxx 文件末尾加一行：
//     REGISTER_MODULE("my_driver", MyDriverModule);
//
//   然后 ModuleManager 自动识别 "my_driver" 模块名
//   无需修改 ModuleManager、CMakeLists.txt、tasks.json
//=============================================================================

#include "module_manager.h"
#include <map>
#include <memory>
#include <functional>

using ModuleCreator = std::function<std::unique_ptr<AppModule>()>;

class ModuleFactory {
public:
    static std::map<std::string, ModuleCreator>& Registry() {
        static std::map<std::string, ModuleCreator> reg;
        return reg;
    }

    static void Register(const std::string& name, ModuleCreator creator) {
        Registry()[name] = std::move(creator);
    }

    static std::unique_ptr<AppModule> Create(const std::string& name) {
        auto it = Registry().find(name);
        if (it != Registry().end())
            return it->second();
        return nullptr;
    }

    static std::vector<std::string> RegisteredNames() {
        std::vector<std::string> names;
        for (const auto& [name, _] : Registry())
            names.push_back(name);
        return names;
    }
};

/// 模块注册宏：在 .cxx 文件末尾调用
/// REGISTER_MODULE("modbus_tcp_master", ModbusTcpMasterModule);
#define REGISTER_MODULE(name, className)                              \
    static bool _reg_##className = []{                               \
        ModuleFactory::Register(name,                                \
            []{ return std::make_unique<className>(); });             \
        return true;                                                  \
    }();

#endif // MODULE_FACTORY_H
