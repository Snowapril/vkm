// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/memory_inspector.h>

#include <vkm/renderer/backend/common/driver.h>

#include <imgui.h>

#include <algorithm>
#include <string>

namespace vkm
{
    namespace
    {
        // Rows beyond this are not worth the vertical space; the count of what was dropped is
        // always shown rather than truncating silently.
        constexpr int kMaxTagRows = 32;

        void drawLabeledBytes(const char* label, uint64_t bytes)
        {
            ImGui::Text("%s: %s", label, formatByteSize(bytes).c_str());
        }

        // "tracked / total" bar with the share as its overlay text.
        void drawShareBar(uint64_t part, uint64_t whole)
        {
            const float fraction = whole > 0 ? static_cast<float>(static_cast<double>(part) / static_cast<double>(whole)) : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.1f%% of %s", fraction * 100.0f, formatByteSize(whole).c_str());
            ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), overlay);
        }

        void drawProcessSection(const VkmMemorySnapshot& snapshot)
        {
            if (!ImGui::CollapsingHeader("Process", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            if (snapshot._process._valid)
            {
                drawLabeledBytes("Resident", snapshot._process._residentBytes);
                ImGui::SameLine();
                ImGui::TextDisabled("(peak %s)", formatByteSize(snapshot._process._peakResidentBytes).c_str());
                drawLabeledBytes("Virtual", snapshot._process._virtualBytes);
            }
            else
            {
                ImGui::TextDisabled("Process memory is not reported on this platform.");
            }

            drawLabeledBytes("Allocator committed", snapshot._mimalloc.currentCommittedBytes);
            ImGui::SameLine();
            ImGui::TextDisabled("(peak %s)", formatByteSize(snapshot._mimalloc.peakCommittedBytes).c_str());
            ImGui::Text("Page faults: %zu", snapshot._mimalloc.pageFaults);
        }

        void drawCpuSection(const VkmMemorySnapshot& snapshot)
        {
            if (!ImGui::CollapsingHeader("CPU: tracked vs actual", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            drawLabeledBytes("Tracked (usable)", snapshot._cpuTrackedUsableBytes);
            ImGui::SameLine();
            ImGui::TextDisabled("(requested %s, %llu live allocations)",
                                formatByteSize(snapshot._cpuTrackedRequestedBytes).c_str(),
                                static_cast<unsigned long long>(snapshot._cpuTrackedLiveCount));

            if (snapshot._process._valid)
            {
                const uint64_t resident = snapshot._process._residentBytes;
                const uint64_t untracked = resident > snapshot._cpuTrackedUsableBytes
                                               ? resident - snapshot._cpuTrackedUsableBytes
                                               : 0;
                drawShareBar(snapshot._cpuTrackedUsableBytes, resident);
                drawLabeledBytes("Untracked remainder", untracked);
                ImGui::SetItemTooltip("Everything the tracker never sees: the binary itself, thread stacks, "
                                      "mapped files, and driver-side allocations -- including GPU resources "
                                      "on unified-memory devices.");
            }
        }

        void drawGpuSection(const VkmMemorySnapshot& snapshot)
        {
            if (!ImGui::CollapsingHeader("GPU: tracked vs actual", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            drawLabeledBytes("Tracked (allocated)", snapshot._gpuTotal.totalAllocatedBytes);
            ImGui::SameLine();
            ImGui::TextDisabled("(requested %s, %u live resources)",
                                formatByteSize(snapshot._gpuTotal.totalRequestedBytes).c_str(),
                                snapshot._gpuTotal.liveCount);

            if (snapshot._gpu._hasDeviceStats)
            {
                drawLabeledBytes("Device allocated", snapshot._gpu._deviceAllocatedBytes);
                if (snapshot._gpu._deviceBudgetBytes > 0)
                {
                    drawShareBar(snapshot._gpu._deviceAllocatedBytes, snapshot._gpu._deviceBudgetBytes);
                }

                const uint64_t overhead = snapshot._gpu._deviceAllocatedBytes > snapshot._gpuTotal.totalAllocatedBytes
                                              ? snapshot._gpu._deviceAllocatedBytes - snapshot._gpuTotal.totalAllocatedBytes
                                              : 0;
                drawLabeledBytes("Driver/allocator overhead", overhead);
                ImGui::SetItemTooltip("Device allocation the engine did not ask for directly: block padding, "
                                      "alignment, and the driver's own bookkeeping.");
            }
            else
            {
                ImGui::TextDisabled("Device memory is not reported by this backend.");
            }

            if (snapshot._gpu._hasPoolStats)
            {
                ImGui::Text("Suballocator: %s used of %s reserved",
                            formatByteSize(snapshot._gpu._poolUsedBytes).c_str(),
                            formatByteSize(snapshot._gpu._poolReservedBytes).c_str());
            }

            ImGui::TextDisabled("On unified-memory devices GPU allocations also count inside the process "
                                "resident figure, so the two are not additive.");
        }

        void drawTagTable(const VkmMemorySnapshot& snapshot)
        {
            if (!ImGui::CollapsingHeader("CPU allocations by tag", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
            if (!ImGui::BeginTable("cpu_tags", 4, kTableFlags, ImVec2(0.0f, 12 * ImGui::GetTextLineHeightWithSpacing())))
            {
                return;
            }

            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Tag");
            ImGui::TableSetupColumn("Live");
            ImGui::TableSetupColumn("Requested");
            ImGui::TableSetupColumn("Usable");
            ImGui::TableHeadersRow();

            // The snapshot already arrives sorted by usable bytes descending.
            const int rowCount = std::min(kMaxTagRows, static_cast<int>(snapshot._cpuTags.size()));
            for (int row = 0; row < rowCount; ++row)
            {
                const TaggedAllocationSummary& tag = snapshot._cpuTags[row];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // Symbolized call sites carry the allocated type in their template arguments,
                // which is the useful part but far wider than the column; the cell clips and
                // the tooltip carries the whole thing.
                const std::string name = formatMemoryTagName(tag);
                ImGui::TextUnformatted(name.c_str());
                if (tag.callSite != nullptr)
                {
                    ImGui::SetItemTooltip("%s", name.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", tag.liveCount);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(tag.requestedBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(tag.usableBytes).c_str());
            }
            ImGui::EndTable();

            if (static_cast<int>(snapshot._cpuTags.size()) > rowCount)
            {
                ImGui::TextDisabled("Showing %d of %zu tags (largest first).", rowCount, snapshot._cpuTags.size());
            }
        }

        void drawCategoryTable(const VkmMemorySnapshot& snapshot)
        {
            if (!ImGui::CollapsingHeader("GPU resources by category", ImGuiTreeNodeFlags_DefaultOpen))
            {
                return;
            }

            constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingStretchProp;
            if (!ImGui::BeginTable("gpu_categories", 4, kTableFlags))
            {
                return;
            }

            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Live");
            ImGui::TableSetupColumn("Requested");
            ImGui::TableSetupColumn("Allocated");
            ImGui::TableHeadersRow();

            for (uint8_t type = 0; type < static_cast<uint8_t>(VkmResourceType::Count); ++type)
            {
                const VkmResourceCategoryUsage& usage = snapshot._gpuByCategory[type];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(vkmResourceTypeName(static_cast<VkmResourceType>(type)));
                ImGui::TableNextColumn();
                ImGui::Text("%u", usage.liveCount);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(usage.totalRequestedBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatByteSize(usage.totalAllocatedBytes).c_str());
            }
            ImGui::EndTable();
        }
    } // namespace

    void VkmMemoryInspector::update(VkmDriverBase* driver, double deltaTime)
    {
        _secondsSinceCapture += deltaTime;
        if (!_refreshRequested && _secondsSinceCapture < static_cast<double>(_refreshIntervalSeconds))
        {
            return;
        }

        _snapshot = captureMemorySnapshot(driver);
        _secondsSinceCapture = 0.0;
        _refreshRequested = false;
    }

    void VkmMemoryInspector::draw()
    {
        if (!_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Memory Inspector", &_visible))
        {
            ImGui::End();
            return;
        }

        if (ImGui::Button("Refresh now"))
        {
            _refreshRequested = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Refresh interval", &_refreshIntervalSeconds, 0.1f, 5.0f, "%.1f s");
        ImGui::SetItemTooltip("Sampling locks the allocator's global tag table, so a very short "
                              "interval slows every allocation in the process down.");
        ImGui::Separator();

        drawProcessSection(_snapshot);
        drawCpuSection(_snapshot);
        drawGpuSection(_snapshot);
        drawTagTable(_snapshot);
        drawCategoryTable(_snapshot);

        ImGui::End();
    }
} // namespace vkm
