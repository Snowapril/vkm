// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    // Reference command deferred recoridng codes from filament(http://github.com/google/filament/)
    class VkmCommandBase
    {
    public:
        static constexpr const size_t VKM_COMMAND_ALIGNMENT = alignof(std::maxalign_t);
    protected:
        using ExecuteCommandType = VkmDispatcher::ExecuteCommandType;
        constexpr explicit VkmCommandBase(ExecuteCommandType command) : _command(command) {}

    public:
        static constexpr size_t align(size_t size) 
        {
            return (size + VKM_COMMAND_ALIGNMENT - 1) & ~VKM_COMMAND_ALIGNMENT;
        }

        inline VkmCommandBase* execute(VkmDriverBase& driver)
        {
            intptr_t next;
            _command(driver, this, &next);
            return reinterpret_cast<VkmCommandBase*>(reinterpret_cast<intptr_t>(this) + next);
        }

        inline ~VkmCommandBase() noexcept = default;

    private:
        ExecuteCommandType _command;
    };

    template<typename T, typename Type, typename D, typename ... ARGS>
    constexpr decltype(auto) invoke(Type T::* m, D&& d, ARGS&& ... args) 
    {
        static_assert(std::is_base_of<T, std::decay_t<D>>::value,
                "member function and object not related");
        return (std::forward<D>(d).*m)(std::forward<ARGS>(args)...);
    }

    template<typename M, typename D, typename T, std::size_t... I>
    constexpr decltype(auto) trampoline(M&& m, D&& d, T&& t, std::index_sequence<I...>) 
    {
        return invoke(std::forward<M>(m), std::forward<D>(d), std::get<I>(std::forward<T>(t))...);
    }

    template<typename M, typename D, typename T>
    constexpr decltype(auto) apply(M&& m, D&& d, T&& t) 
    {
        return trampoline(std::forward<M>(m), std::forward<D>(d), std::forward<T>(t), 
                        std::make_index_sequence< std::tuple_size<std::remove_reference_t<T>>::value >{});
    }

    template <typename... Args>
    struct CommandType;

    template <typename... Args>
    struct CommandType<void (VkmDriverBase::*)(Args...)>
    {
        template <typename void (VkmDriverBase::*)(Args...)>
        class Command
        {
        public:
            using SavedParameters = std::tuple<std::remove_reference_t<ARGS>...>;
            SavedParameters mArgs;
        
            inline Command(Command&& rhs) = default;

        public:
            
            void execute()
            {

            }

        private:
            
        };
    };

    #define COMMAND_TYPE(methodName) CommandType<decltype(&VkmDriverBase::methodName)>::Command<&VkmDriverBase::methodName>

    class VkmCommandStream
    {
    public:
        VkmCommandStream();
        ~VkmCommandStream();

    public:
        #define DECL_DRIVER_API(methodName, paramsDecl, params)                                         \
        inline void methodName(paramsDecl) {                                                        \
            using Cmd = COMMAND_TYPE(methodName);                                                   \
            void* const p = allocateCommand(CommandBase::align(sizeof(Cmd)));                       \
            new(p) Cmd(mDispatcher.methodName##_, APPLY(std::move, params));                        \
        }

        #include "vkm/renderer/backend/common/driver_api.inc"

    private:

    };
}