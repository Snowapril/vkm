// ImGui vertex/fragment pair mirroring upstream imgui_impl_metal.mm's shader, retargeted
// to bind resources by plain buffer(N)/texture(N)/sampler(N) index (an MTL4ArgumentTable
// binds to the same indices; the shader syntax itself is unchanged).
//
// Precompiled to a metallib at build time and embedded via EmbedBinary.cmake, then loaded
// at runtime with -[MTLDevice newLibraryWithData:] (see metal_imgui_renderer.mm). A binary
// library serializes into Xcode GPU captures; a source library compiled through
// MTL4Compiler does not, which made .gputrace replay crash.
#include <metal_stdlib>
using namespace metal;

struct Uniforms { float4x4 projectionMatrix; };

struct VertexIn { float2 position; float2 texCoord; uchar4 color; };

struct VertexOut
{
    float4 position [[position]];
    float2 texCoord;
    float4 color;
};

vertex VertexOut vkm_imgui_vertex(uint vertexID [[vertex_id]],
                                   device const VertexIn* vertices [[buffer(0)]],
                                   constant Uniforms& uniforms [[buffer(1)]])
{
    VertexIn in = vertices[vertexID];
    VertexOut out;
    out.position = uniforms.projectionMatrix * float4(in.position, 0.0, 1.0);
    out.texCoord = in.texCoord;
    out.color = float4(in.color) / float4(255.0);
    return out;
}

fragment float4 vkm_imgui_fragment(VertexOut in [[stage_in]],
                                    texture2d<float> tex [[texture(0)]],
                                    sampler texSampler [[sampler(0)]])
{
    return in.color * tex.sample(texSampler, in.texCoord);
}
