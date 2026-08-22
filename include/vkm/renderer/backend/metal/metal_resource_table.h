// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/resource_table.h>

#include <cstdint>
#include <vector>

#import <Metal/MTLTypes.h>

@protocol MTL4ArgumentTable;

namespace vkm
{
    /*
    * @brief Metal set 2: discrete argument-table bindings, resolved once and replayed on each bind.
    *
    * @details Metal has no descriptor set to allocate, an argument table belonging to the encoder
    * rather than to the pass, so there is nothing to write up front. This table stores the resolved
    * form of each entry -- a GPU address, or an MTLResourceID -- at the Metal index vkm-compiler
    * pinned it to, so binding is a short loop of setAddress:/setTexture:/setSamplerState: with no
    * handle lookups in the recording path. Resolving at build time is what makes the base class's
    * immutability meaningful here: a resource that moved or was recreated needs a rebuilt table.
    */
    class VkmResourceTableMetal : public VkmResourceTableBase
    {
    public:
        explicit VkmResourceTableMetal(VkmDriverBase* driver);
        ~VkmResourceTableMetal() override;

        // Applies every resolved entry to `argumentTable`. Called from the command buffer, which
        // owns the encoder the table is attached to.
        void applyTo(id<MTL4ArgumentTable> argumentTable) const;

    protected:
        bool createInner(const std::vector<VkmTableResourceEntry>& entries, std::string* outError) override final;
        void destroyInner() override final;

    private:
        enum class BindingKind : uint8_t
        {
            Address,
            Texture,
            SamplerState,
        };

        struct ResolvedBinding
        {
            BindingKind kind;
            uint32_t index; // Metal index, per kVkmMetalPerPass*IndexBase + declared binding
            uint64_t address; // Address only (MTLGPUAddress is a uint64_t typedef)
            MTLResourceID resourceId; // Texture / SamplerState only
        };

        std::vector<ResolvedBinding> _bindings;
    };
} // namespace vkm
