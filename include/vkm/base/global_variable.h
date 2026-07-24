// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief Type-erased base for a self-registering global variable (cvar).
    * @details A GlobalVariable<T> registers itself with GlobalVariableManager on
    * construction so the manager can override it from a string (command line) by name.
    * Typed value access is through the GlobalVariable<T> subclass, not this base.
    */
    class GlobalVariableBase
    {
    public:
        explicit GlobalVariableBase(const char* name);
        virtual ~GlobalVariableBase();

        GlobalVariableBase(const GlobalVariableBase&) = delete;
        GlobalVariableBase& operator=(const GlobalVariableBase&) = delete;

        inline const char* getName() const { return _name; }

        /*
        * @brief Parse `valueStr` into this variable's type and assign it. Returns false and
        * leaves the value unchanged when `valueStr` is not a valid value for the type.
        */
        virtual bool setFromString(const std::string& valueStr) = 0;

    protected:
        const char* _name;
    };

    /*
    * @brief Registry of every GlobalVariableBase, and the command-line override entry point.
    * @details Immortal singleton (placement-new into static storage, never destroyed -- the
    * same idiom as LoggerManager/MemoryTracker) so it is valid during the static
    * initialization that registers global variables, before main().
    */
    class GlobalVariableManager
    {
    public:
        static GlobalVariableManager& singleton();

        GlobalVariableManager(const GlobalVariableManager&) = delete;
        GlobalVariableManager& operator=(const GlobalVariableManager&) = delete;

        /*
        * @brief Register a variable so command-line overrides can reach it by name. Called
        * from GlobalVariable<T>'s constructor at static-init time.
        */
        void registerVariable(GlobalVariableBase* variable);

        /*
        * @brief Stash raw command-line tokens (e.g. cxxopts' unmatched args) to be applied
        * later by applyCommandLineOverrides(). Kept separate from apply so overrides can be
        * collected during argument parsing but only applied (and logged) once the logger is up.
        */
        void setCommandLineOverrides(const std::vector<std::string>& args);

        /*
        * @brief Apply the stashed "--<name>=<value>" tokens to matching variables. Tokens not
        * of that form, or whose <name> matches no registered variable, are ignored (they may
        * belong to another subsystem). A malformed <value> for a matched <name> is logged and
        * skipped. Returns the number of overrides successfully applied and clears the stash.
        */
        size_t applyCommandLineOverrides();

    private:
        GlobalVariableManager() = default;
        ~GlobalVariableManager() = default;

        std::vector<GlobalVariableBase*> _variables;
        std::vector<std::string> _pendingOverrides;
    };

    /*
    * @brief A translation-unit-local global variable of type T that self-registers for
    * command-line override. Declare via VKM_GLOBAL_VARIABLE. Supported types: bool, int32_t,
    * uint32_t, float, std::string. Overrides are applied once at startup before any render
    * thread starts, so reads are unsynchronized by design.
    */
    template <typename T>
    class GlobalVariable final : public GlobalVariableBase
    {
    public:
        GlobalVariable(const char* name, T defaultValue)
            : GlobalVariableBase(name), _value(defaultValue)
        {
            GlobalVariableManager::singleton().registerVariable(this);
        }

        inline const T& get() const { return _value; }
        inline void set(const T& value) { _value = value; }
        inline operator const T&() const { return _value; }

        bool setFromString(const std::string& valueStr) override;

    private:
        T _value;
    };

    // setFromString is fully specialized per type in global_variable.cpp; these declarations
    // must precede the extern template instantiations below (an instantiation before the
    // specialization is declared is ill-formed).
    template <> bool GlobalVariable<bool>::setFromString(const std::string& valueStr);
    template <> bool GlobalVariable<int32_t>::setFromString(const std::string& valueStr);
    template <> bool GlobalVariable<uint32_t>::setFromString(const std::string& valueStr);
    template <> bool GlobalVariable<float>::setFromString(const std::string& valueStr);
    template <> bool GlobalVariable<std::string>::setFromString(const std::string& valueStr);

    // Only these instantiations exist (defined in global_variable.cpp); extern template keeps
    // other translation units from implicitly instantiating setFromString / the vtable.
    extern template class GlobalVariable<bool>;
    extern template class GlobalVariable<int32_t>;
    extern template class GlobalVariable<uint32_t>;
    extern template class GlobalVariable<float>;
    extern template class GlobalVariable<std::string>;
} // namespace vkm

/*
* @brief Declare a global variable in a .cpp/.mm at file scope. `name` is the identifier used
* to access it (e.g. gv_metal4_pso) and also the command-line override key (--<name>=<value>).
* The variable has internal linkage, so each translation unit that declares a given name gets
* its own instance (all instances of the same name are overridden together).
*
*   VKM_GLOBAL_VARIABLE(int32_t, gv_metal4_pso, 0);
*   ... if (gv_metal4_pso.get() == 1) ...
*   ./triangle --gv_metal4_pso=1
*/
#define VKM_GLOBAL_VARIABLE(Type, name, defaultValue) \
    static ::vkm::GlobalVariable<Type> name{ #name, defaultValue }
