#include "NativeEngine.h"

#include "RuntimeImpl.h"
#include "NapiBridge.h"
#include "ShaderCompiler.h"
#include "Console.h"
#include "NativeEngineImpl.h"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

// TODO: this needs to be fixed in bgfx
namespace bgfx
{
    uint16_t attribToId(Attrib::Enum _attr);
}

#define BGFX_UNIFORM_FRAGMENTBIT UINT8_C(0x10) // Copy-pasta from bgfx_p.h
#define BGFX_UNIFORM_SAMPLERBIT  UINT8_C(0x20) // Copy-pasta from bgfx_p.h

#include <bimg/bimg.h>
#include <bimg/decode.h>
#include <bimg/encode.h>

#include <bx/math.h>
#include <bx/readerwriter.h>

#include <queue>
#include <regex>
#include <sstream>

namespace babylon
{
    namespace
    {
        template<typename AppendageT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const AppendageT appendage)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(&appendage);
            auto stride = static_cast<std::ptrdiff_t>(sizeof(AppendageT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename AppendageT = std::string&>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const std::string& string)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(string.data());
            auto stride = static_cast<std::ptrdiff_t>(string.length());
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        template<typename ElementT>
        inline void AppendBytes(std::vector<uint8_t>& bytes, const gsl::span<ElementT>& data)
        {
            auto ptr = reinterpret_cast<const uint8_t*>(data.data());
            auto stride = static_cast<std::ptrdiff_t>(data.size() * sizeof(ElementT));
            bytes.insert(bytes.end(), ptr, ptr + stride);
        }

        void FlipYInImageBytes(gsl::span<uint8_t> bytes, size_t rowCount, size_t rowPitch)
        {
            std::vector<uint8_t> buffer{};
            buffer.reserve(rowPitch);

            for (size_t row = 0; row < rowCount / 2; row++)
            {
                auto frontPtr = bytes.data() + (row * rowPitch);
                auto backPtr = bytes.data() + ((rowCount - row - 1) * rowPitch);

                std::memcpy(buffer.data(), frontPtr, rowPitch);
                std::memcpy(frontPtr, backPtr, rowPitch);
                std::memcpy(backPtr, buffer.data(), rowPitch);
            }
        }

        void AppendUniformBuffer(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::Resource& uniformBuffer, bool isFragment)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            const spirv_cross::SPIRType& type = compiler.get_type(uniformBuffer.base_type_id);
            for (uint32_t index = 0; index < type.member_types.size(); ++index)
            {
                auto name = compiler.get_member_name(uniformBuffer.base_type_id, index);
                auto offset = compiler.get_member_decoration(uniformBuffer.base_type_id, index, spv::DecorationOffset);
                auto memberType = compiler.get_type(type.member_types[index]);

                bgfx::UniformType::Enum bgfxType;
                uint16_t regCount;

                if (memberType.basetype != spirv_cross::SPIRType::Float)
                {
                    throw std::exception(); // Not supported
                }

                if (memberType.columns == 1 && 1 <= memberType.vecsize && memberType.vecsize <= 4)
                {
                    bgfxType = bgfx::UniformType::Vec4;
                    regCount = 1;
                }
                else if (memberType.columns == 4 && memberType.vecsize == 4)
                {
                    bgfxType = bgfx::UniformType::Mat4;
                    regCount = 4;
                }
                else
                {
                    throw std::exception();
                }

                for (const auto size : memberType.array)
                {
                    regCount *= size;
                }

                AppendBytes(bytes, static_cast<uint8_t>(name.size()));
                AppendBytes(bytes, name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfxType | fragmentBit));
                AppendBytes(bytes, static_cast<uint8_t>(0)); // Value "num" not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint16_t>(offset));
                AppendBytes(bytes, static_cast<uint16_t>(regCount));
            }
        }

        void AppendSamplers(std::vector<uint8_t>& bytes, const spirv_cross::Compiler& compiler, const spirv_cross::SmallVector<spirv_cross::Resource>& samplers, bool isFragment, std::unordered_map<std::string, UniformInfo>& cache)
        {
            const uint8_t fragmentBit = (isFragment ? BGFX_UNIFORM_FRAGMENTBIT : 0);

            for (const spirv_cross::Resource& sampler : samplers)
            {
                AppendBytes(bytes, static_cast<uint8_t>(sampler.name.size()));
                AppendBytes(bytes, sampler.name);
                AppendBytes(bytes, static_cast<uint8_t>(bgfx::UniformType::Sampler | BGFX_UNIFORM_SAMPLERBIT));

                // These values (num, regIndex, regCount) are not used by D3D11 pipeline.
                AppendBytes(bytes, static_cast<uint8_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));
                AppendBytes(bytes, static_cast<uint16_t>(0));

                cache[sampler.name].Stage = compiler.get_decoration(sampler.id, spv::DecorationBinding);
            }
        }

        void CacheUniformHandles(bgfx::ShaderHandle shader, std::unordered_map<std::string, UniformInfo>& cache)
        {
            const auto MAX_UNIFORMS = 256;
            bgfx::UniformHandle uniforms[MAX_UNIFORMS];
            auto numUniforms = bgfx::getShaderUniforms(shader, uniforms, MAX_UNIFORMS);

            bgfx::UniformInfo info{};
            for (uint8_t idx = 0; idx < numUniforms; idx++)
            {
                bgfx::getUniformInfo(uniforms[idx], info);
                cache[info.name].Handle = uniforms[idx];
            }
        }

        enum class WebGLAttribType
        {
            BYTE = 5120,
            UNSIGNED_BYTE = 5121,
            SHORT = 5122,
            UNSIGNED_SHORT = 5123,
            INT = 5124,
            UNSIGNED_INT = 5125,
            FLOAT = 5126
        };

        bgfx::AttribType::Enum ConvertAttribType(WebGLAttribType type)
        {
            switch (type)
            {
            case WebGLAttribType::UNSIGNED_BYTE:    return bgfx::AttribType::Uint8;
            case WebGLAttribType::SHORT:            return bgfx::AttribType::Int16;
            case WebGLAttribType::FLOAT:            return bgfx::AttribType::Float;
            default: // avoid "warning: 4 enumeration values not handled"
                throw std::exception();
                break;
            }
        }

        // Must match constants.ts in Babylon.js.
        constexpr std::array<uint64_t, 11> ALPHA_MODE
        {
            // ALPHA_DISABLE
            0x0,

            // ALPHA_ADD: SRC ALPHA * SRC + DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE),

            // ALPHA_COMBINE: SRC ALPHA * SRC + (1 - SRC ALPHA) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_SUBTRACT: DEST - SRC * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_MULTIPLY: SRC * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_MAXIMIZED: SRC ALPHA * SRC + (1 - SRC) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_ONEONE: SRC + DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE),

            // ALPHA_PREMULTIPLIED: SRC + (1 - SRC ALPHA) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE),

            // ALPHA_PREMULTIPLIED_PORTERDUFF: SRC + (1 - SRC ALPHA) * DEST, (1 - SRC ALPHA) * DEST ALPHA
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA),

            // ALPHA_INTERPOLATE: CST * SRC + (1 - CST) * DEST
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_INV_FACTOR, BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_INV_FACTOR),

            // ALPHA_SCREENMODE: SRC + (1 - SRC) * DEST, SRC ALPHA + (1 - SRC ALPHA) * DEST ALPHA
            BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_COLOR, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA),
        };

    class NativeEngine::Impl final
    {
    public:
        Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl, uint32_t width, uint32_t height);

        void Initialize(Napi::Env& env);
        
        void UpdateSize(float width, float height);
        void UpdateRenderTarget();
        void Suspend();

    private:
        void InitializeRendering();

        using EngineDefiner = NativeEngineDefiner<NativeEngine::Impl>;
        friend EngineDefiner;

        struct VertexArray final
        constexpr std::array<bgfx::TextureFormat::Enum, 2> TEXTURE_FORMAT
        {
            bgfx::TextureFormat::RGBA8,
            bgfx::TextureFormat::RGBA32F
        };

    }

    NativeEngine::Impl::Impl(void* nativeWindowPtr, RuntimeImpl& runtimeImpl, uint32_t width, uint32_t height)
        : m_runtimeImpl{ runtimeImpl }
        , m_currentProgram{ nullptr }
        , m_size{ width, height }
        , m_engineState{ BGFX_STATE_DEFAULT }
        , m_nativeWindow{ nativeWindowPtr }
    {
        
    }

    void NativeEngine::Impl::InitializeRendering()
    {
        bgfx::Init init{};
        init.platformData.nwh = m_nativeWindow;
        bgfx::setPlatformData(init.platformData);

        init.type = bgfx::RendererType::Direct3D11;
        init.resolution.width = m_size.Width;
        init.resolution.height = m_size.Height;
        init.resolution.reset = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
        bgfx::init(init);

        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x443355FF, 1.0f, 0);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);
        EngineDefiner::Define(env, this);
    }

    void NativeEngine::Impl::Initialize(Napi::Env& env)
    {
        // Rendering is initialized here because for OpenGL(ES) initialization must happen in the same thread for rendering
        // OpenGL context is associated with 1 thread only at a time. To Associate it with another thread, MakeCurrentContext must be used
        // but bgfx doesn't expose it.
        InitializeRendering();
    }
    
    void NativeEngine::Impl::UpdateSize(float width, float height)
    {
        auto w = static_cast<uint32_t>(width);
        auto h = static_cast<uint32_t>(height);

        if (w != m_size.Width || h != m_size.Height)
        {
            m_size = { w, h };
            UpdateRenderTarget();
        }
    }

    void NativeEngine::Impl::UpdateRenderTarget()
    {
        bgfx::reset(m_size.Width, m_size.Height, BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4);
        bgfx::setViewRect(0, 0, 0, m_size.Width, m_size.Height);
    }

    FrameBufferManager& NativeEngine::Impl::GetFrameBufferManager()
    {
        return m_frameBufferManager;
    }

    // NativeEngine definitions
    Napi::Value NativeEngine::Impl::GetEngine(const Napi::CallbackInfo& info)
    {
        return Napi::External<NativeEngine::Impl>::New(info.Env(), this);
    }

    void NativeEngine::Impl::RequestAnimationFrame(const Napi::CallbackInfo& info)
    {
        DispatchAnimationFrameAsync(Napi::Persistent(info[0].As<Napi::Function>()));
    }

    Napi::Value NativeEngine::Impl::CreateVertexArray(const Napi::CallbackInfo& info)
    {
        return Napi::External<VertexArray>::New(info.Env(), new VertexArray{});
    }

    void NativeEngine::Impl::DeleteVertexArray(const Napi::CallbackInfo& info)
    {
        delete info[0].As<Napi::External<VertexArray>>().Data();
    }

    void NativeEngine::Impl::BindVertexArray(const Napi::CallbackInfo& info)
    {
        const auto& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());

        bgfx::setIndexBuffer(vertexArray.indexBuffer.handle);

        const auto& vertexBuffers = vertexArray.vertexBuffers;
        for (uint8_t index = 0; index < vertexBuffers.size(); ++index)
        {
            const auto& vertexBuffer = vertexBuffers[index];
            bgfx::setVertexBuffer(index, vertexBuffer.handle, vertexBuffer.startVertex, UINT32_MAX, vertexBuffer.declHandle);
        }
    }

    Napi::Value NativeEngine::Impl::CreateIndexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::TypedArray data = info[0].As<Napi::TypedArray>();
        const bgfx::Memory* ref = bgfx::copy(data.As<Napi::Uint8Array>().Data(), static_cast<uint32_t>(data.ByteLength()));
        const uint16_t flags = data.TypedArrayType() == napi_typedarray_type::napi_uint16_array ? 0 : BGFX_BUFFER_INDEX32;
        const bgfx::IndexBufferHandle handle = bgfx::createIndexBuffer(ref, flags);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void NativeEngine::Impl::DeleteIndexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void NativeEngine::Impl::RecordIndexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::IndexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        vertexArray.indexBuffer.handle = handle;
    }

    Napi::Value NativeEngine::Impl::CreateVertexBuffer(const Napi::CallbackInfo& info)
    {
        const Napi::Uint8Array data = info[0].As<Napi::Uint8Array>();

        // HACK: Create an empty valid vertex decl which will never be used. Consider fixing in bgfx.
        bgfx::VertexDecl decl;
        decl.begin();
        decl.m_stride = 1;
        decl.end();

        const bgfx::Memory* ref = bgfx::copy(data.Data(), static_cast<uint32_t>(data.ByteLength()));
        const bgfx::VertexBufferHandle handle = bgfx::createVertexBuffer(ref, decl);
        return Napi::Value::From(info.Env(), static_cast<uint32_t>(handle.idx));
    }

    void NativeEngine::Impl::DeleteVertexBuffer(const Napi::CallbackInfo& info)
    {
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value()) };
        bgfx::destroy(handle);
    }

    void NativeEngine::Impl::RecordVertexBuffer(const Napi::CallbackInfo& info)
    {
        VertexArray& vertexArray = *(info[0].As<Napi::External<VertexArray>>().Data());
        const bgfx::VertexBufferHandle handle{ static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value()) };
        const uint32_t location = info[2].As<Napi::Number>().Uint32Value();
        const uint32_t byteOffset = info[3].As<Napi::Number>().Uint32Value();
        const uint32_t byteStride = info[4].As<Napi::Number>().Uint32Value();
        const uint32_t numElements = info[5].As<Napi::Number>().Uint32Value();
        const uint32_t type = info[6].As<Napi::Number>().Uint32Value();
        const bool normalized = info[7].As<Napi::Boolean>().Value();

        bgfx::VertexDecl decl;
        decl.begin();
        const bgfx::Attrib::Enum attrib = static_cast<bgfx::Attrib::Enum>(location);
        const bgfx::AttribType::Enum attribType = ConvertAttribType(static_cast<WebGLAttribType>(type));
        decl.add(attrib, numElements, attribType, normalized);
        decl.m_stride = static_cast<uint16_t>(byteStride);
        decl.end();

        vertexArray.vertexBuffers.push_back({ std::move(handle), byteOffset / byteStride, bgfx::createVertexDecl(decl) });
    }

    Napi::Value NativeEngine::Impl::CreateProgram(const Napi::CallbackInfo& info)
    {
        const auto vertexSource = info[0].As<Napi::String>().Utf8Value();
        // TODO: This is a HACK to account for the fact that DirectX and OpenGL disagree about the vertical orientation of screen space.
        // Remove this ASAP when we have a more long-term plan to account for this behavior.
        const auto fragmentSource = std::regex_replace(info[1].As<Napi::String>().Utf8Value(), std::regex("dFdy\\("), "-dFdy(");

        auto programData = new ProgramData();

        std::vector<uint8_t> vertexBytes{};
        std::vector<uint8_t> fragmentBytes{};
        std::unordered_map<std::string, uint32_t> attributeLocations;

        m_shaderCompiler.Compile(vertexSource, fragmentSource, [&](ShaderCompiler::ShaderInfo vertexShaderInfo, ShaderCompiler::ShaderInfo fragmentShaderInfo)
        {
            constexpr uint8_t BGFX_SHADER_BIN_VERSION = 6;

            // These hashes are generated internally by BGFX's custom shader compilation pipeline,
            // which we don't have access to.  Fortunately, however, they aren't used for anything
            // crucial; they just have to match.
            constexpr uint32_t vertexOutputsHash = 0xBAD1DEA;
            constexpr uint32_t fragmentInputsHash = vertexOutputsHash;

            {
                const spirv_cross::Compiler& compiler = *vertexShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(vertexBytes, BX_MAKEFOURCC('V', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(vertexBytes, vertexOutputsHash);
                AppendBytes(vertexBytes, fragmentInputsHash);

                AppendBytes(vertexBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(vertexBytes, compiler, uniformBuffer, false);
                AppendSamplers(vertexBytes, compiler, samplers, false, programData->VertexUniformNameToInfo);

                AppendBytes(vertexBytes, static_cast<uint32_t>(vertexShaderInfo.Bytes.size()));
                AppendBytes(vertexBytes, vertexShaderInfo.Bytes);
                AppendBytes(vertexBytes, static_cast<uint8_t>(0));

                AppendBytes(vertexBytes, static_cast<uint8_t>(resources.stage_inputs.size()));
                for (const spirv_cross::Resource& stageInput : resources.stage_inputs)
                {
                    const uint32_t location = compiler.get_decoration(stageInput.id, spv::DecorationLocation);
                    AppendBytes(vertexBytes, bgfx::attribToId(static_cast<bgfx::Attrib::Enum>(location)));
                    // name attribute has been preprocessed to make link between bgfx and the shader (position -> a_position)
                    // but bjs doesn't know the attribute names. So the make the reverse here
                    std::string attributeName = stageInput.name;
                    if (attributeName == "a_position")
                        attributeName = "position";
                    else if (attributeName == "a_normal")
                        attributeName = "normal";
                    else if (attributeName == "a_tangent")
                        attributeName = "tangent";
                    else if (attributeName == "a_color")
                        attributeName = "color";
                    else if (attributeName == "a_index")
                        attributeName = "matricesIndices";
                    else if (attributeName == "a_weight")
                        attributeName = "matricesWeights";
                    attributeLocations[attributeName] = location;
                }

                AppendBytes(vertexBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }

            {
                const spirv_cross::Compiler& compiler = *fragmentShaderInfo.Compiler;
                const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                assert(resources.uniform_buffers.size() == 1);
                const spirv_cross::Resource& uniformBuffer = resources.uniform_buffers[0];
                const spirv_cross::SmallVector<spirv_cross::Resource>& samplers = resources.separate_samplers;
                size_t numUniforms = compiler.get_type(uniformBuffer.base_type_id).member_types.size() + samplers.size();

                AppendBytes(fragmentBytes, BX_MAKEFOURCC('F', 'S', 'H', BGFX_SHADER_BIN_VERSION));
                AppendBytes(fragmentBytes, vertexOutputsHash);
                AppendBytes(fragmentBytes, fragmentInputsHash);

                AppendBytes(fragmentBytes, static_cast<uint16_t>(numUniforms));
                AppendUniformBuffer(fragmentBytes, compiler, uniformBuffer, true);
                AppendSamplers(fragmentBytes, compiler, samplers, true, programData->FragmentUniformNameToInfo);

                AppendBytes(fragmentBytes, static_cast<uint32_t>(fragmentShaderInfo.Bytes.size()));
                AppendBytes(fragmentBytes, fragmentShaderInfo.Bytes);
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                // Fragment shaders don't have attributes.
                AppendBytes(fragmentBytes, static_cast<uint8_t>(0));

                AppendBytes(fragmentBytes, static_cast<uint16_t>(compiler.get_declared_struct_size(compiler.get_type(uniformBuffer.base_type_id))));
            }
        });

        auto vertexShader = bgfx::createShader(bgfx::copy(vertexBytes.data(), static_cast<uint32_t>(vertexBytes.size())));
        CacheUniformHandles(vertexShader, programData->VertexUniformNameToInfo);
        programData->AttributeLocations = std::move(attributeLocations);

        auto fragmentShader = bgfx::createShader(bgfx::copy(fragmentBytes.data(), static_cast<uint32_t>(fragmentBytes.size())));
        CacheUniformHandles(fragmentShader, programData->FragmentUniformNameToInfo);

        programData->Program = bgfx::createProgram(vertexShader, fragmentShader, true);

        auto finalizer = [](Napi::Env, ProgramData* data)
        {
            delete data;
        };

        return Napi::External<ProgramData>::New(info.Env(), programData, finalizer);
    }

    Napi::Value NativeEngine::Impl::GetUniforms(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        auto length = names.Length();
        auto uniforms = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();

            auto vertexFound = program->VertexUniformNameToInfo.find(name);
            auto fragmentFound = program->FragmentUniformNameToInfo.find(name);

            if (vertexFound != program->VertexUniformNameToInfo.end())
            {
                uniforms[index] = Napi::External<UniformInfo>::New(info.Env(), &vertexFound->second);
            }
            else if (fragmentFound != program->FragmentUniformNameToInfo.end())
            {
                uniforms[index] = Napi::External<UniformInfo>::New(info.Env(), &fragmentFound->second);
            }
            else
            {
                uniforms[index] = info.Env().Null();
            }
        }

        return uniforms;
    }

    Napi::Value NativeEngine::Impl::GetAttributes(const Napi::CallbackInfo& info)
    {
        const auto program = info[0].As<Napi::External<ProgramData>>().Data();
        const auto names = info[1].As<Napi::Array>();

        const auto& attributeLocations = program->AttributeLocations;

        auto length = names.Length();
        auto attributes = Napi::Array::New(info.Env(), length);
        for (uint32_t index = 0; index < length; ++index)
        {
            const auto name = names[index].As<Napi::String>().Utf8Value();
            const auto it = attributeLocations.find(name);
            int location = (it == attributeLocations.end() ? -1 : gsl::narrow_cast<int>(it->second));
            attributes[index] = Napi::Value::From(info.Env(), location);
        }

        return attributes;
    }

    void NativeEngine::Impl::SetProgram(const Napi::CallbackInfo& info)
    {
        auto program = info[0].As<Napi::External<ProgramData>>().Data();
        m_currentProgram = program;
    }

    void NativeEngine::Impl::SetState(const Napi::CallbackInfo& info)
    {
        const auto culling = info[0].As<Napi::Boolean>().Value();
        const auto reverseSide = info[2].As<Napi::Boolean>().Value();

        m_engineState &= ~BGFX_STATE_CULL_MASK;
        if (reverseSide)
        {
            m_engineState &= ~BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CW;
            }
        }
        else
        {
            m_engineState |= BGFX_STATE_FRONT_CCW;

            if (culling)
            {
                m_engineState |= BGFX_STATE_CULL_CCW;
            }
        }

        // TODO: zOffset
        const auto zOffset = info[1].As<Napi::Number>().FloatValue();

        bgfx::setState(m_engineState);
    }

    void NativeEngine::Impl::SetZOffset(const Napi::CallbackInfo& info)
    {
        const auto zOffset = info[0].As<Napi::Number>().FloatValue();

        // STUB: Stub.
    }

    Napi::Value NativeEngine::Impl::GetZOffset(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void NativeEngine::Impl::SetDepthTest(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    Napi::Value NativeEngine::Impl::GetDepthWrite(const Napi::CallbackInfo& info)
    {
        // STUB: Stub.
        return{};
    }

    void NativeEngine::Impl::SetDepthWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetColorWrite(const Napi::CallbackInfo& info)
    {
        const auto enable = info[0].As<Napi::Boolean>().Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetBlendMode(const Napi::CallbackInfo& info)
    {
        const auto blendMode = static_cast<BlendMode>(info[0].As<Napi::Number>().Int32Value());

        m_engineState &= ~BGFX_STATE_BLEND_MASK;
        m_engineState |= ALPHA_MODE[blendMode];

        bgfx::setState(m_engineState);
    }

    void NativeEngine::Impl::SetMatrix(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto matrix = info[1].As<Napi::Float32Array>();

        const size_t elementLength = matrix.ElementLength();
        assert(elementLength == 16);

        m_currentProgram->SetUniform(uniformData->Handle, gsl::make_span(matrix.Data(), elementLength));
    }

    void NativeEngine::Impl::SetIntArray(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetIntArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const int> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto array = info[1].As<Napi::Float32Array>();

        size_t elementLength = array.ElementLength();

        m_scratch.clear();
        for (size_t index = 0; index < elementLength; ++index)
        {
            const float values[] = { array[index], 0.0f, 0.0f, 0.0f };
            m_scratch.insert(m_scratch.end(), values, values + 4);
        }

        m_currentProgram->SetUniform(uniformData->Handle, m_scratch, elementLength);
    }

    void NativeEngine::Impl::SetFloatArray2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetFloatArray4(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> array

        assert(false);
    }

    void NativeEngine::Impl::SetMatrices(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto matricesArray = info[1].As<Napi::Float32Array>();

        const size_t elementLength = matricesArray.ElementLength();
        assert(elementLength % 16 == 0);

        m_currentProgram->SetUniform(uniformData->Handle, gsl::span(matricesArray.Data(), elementLength), elementLength / 16);
    }

    void NativeEngine::Impl::SetMatrix3x3(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void NativeEngine::Impl::SetMatrix2x2(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, gsl::span<const float> matrix

        assert(false);
    }

    void NativeEngine::Impl::SetFloat(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f,
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat2(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            0.0f,
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat3(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            0.0f
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetFloat4(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const float values[] =
        {
            info[1].As<Napi::Number>().FloatValue(),
            info[2].As<Napi::Number>().FloatValue(),
            info[3].As<Napi::Number>().FloatValue(),
            info[4].As<Napi::Number>().FloatValue()
        };

        m_currentProgram->SetUniform(uniformData->Handle, values);
    }

    void NativeEngine::Impl::SetBool(const Napi::CallbackInfo& info)
    {
        // args: ShaderProperty property, bool value

        assert(false);
    }

    Napi::Value NativeEngine::Impl::CreateTexture(const Napi::CallbackInfo& info)
    {
        return Napi::External<TextureData>::New(info.Env(), new TextureData());
    }

    void NativeEngine::Impl::LoadTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto buffer = info[1].As<Napi::ArrayBuffer>();
        const auto mipMap = info[2].As<Napi::Boolean>().Value();

        textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.Data(), static_cast<uint32_t>(buffer.ByteLength())));
        auto& image = *textureData->Images.front();

        textureData->Texture = bgfx::createTexture2D(
            image.m_width,
            image.m_height,
            false, // TODO: generate mipmaps when requested
            1,
            static_cast<bgfx::TextureFormat::Enum>(image.m_format),
            0,
            bgfx::makeRef(image.m_data, image.m_size));
    }

    void NativeEngine::Impl::LoadCubeTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto mipLevelsArray = info[1].As<Napi::Array>();
        const auto flipY = info[2].As<Napi::Boolean>().Value();

        std::vector<std::vector<bimg::ImageContainer*>> images{};
        images.reserve(mipLevelsArray.Length());

        uint32_t totalSize = 0;

        for (uint32_t mipLevel = 0; mipLevel < mipLevelsArray.Length(); mipLevel++)
        {
            const auto facesArray = mipLevelsArray[mipLevel].As<Napi::Array>();

            images.emplace_back().reserve(facesArray.Length());

            for (uint32_t face = 0; face < facesArray.Length(); face++)
            {
                const auto image = facesArray[face].As<Napi::TypedArray>();
                auto buffer = gsl::make_span(static_cast<uint8_t*>(image.ArrayBuffer().Data()) + image.ByteOffset(), image.ByteLength());

                textureData->Images.push_back(bimg::imageParse(&m_allocator, buffer.data(), static_cast<uint32_t>(buffer.size())));
                images.back().push_back(textureData->Images.back());
                totalSize += static_cast<uint32_t>(images.back().back()->m_size);
            }
        }

        auto allPixels = bgfx::alloc(totalSize);

        auto ptr = allPixels->data;
        for (uint32_t face = 0; face < images.front().size(); face++)
        {
            for (uint32_t mipLevel = 0; mipLevel < images.size(); mipLevel++)
            {
                const auto image = images[mipLevel][face];

                std::memcpy(ptr, image->m_data, image->m_size);

                if (flipY)
                {
                    FlipYInImageBytes(gsl::make_span(ptr, image->m_size), image->m_height, image->m_size / image->m_height);
                }

                ptr += image->m_size;
            }
        }

        bgfx::TextureFormat::Enum format{};
        switch (images.front().front()->m_format)
        {
            case bimg::TextureFormat::RGBA8:
            {
                format = bgfx::TextureFormat::RGBA8;
                break;
            }
            case bimg::TextureFormat::RGB8:
            {
                format = bgfx::TextureFormat::RGB8;
                break;
            }
            default:
            {
                throw std::exception();
            }
        }

        textureData->Texture = bgfx::createTextureCube(
            images.front().front()->m_width,         // Side size
            true,                                           // Has mips
            1,                                              // Number of layers
            format,                                         // Self-explanatory
            0x0,                                            // Flags
            allPixels);                                     // Memory
    }

    Napi::Value NativeEngine::Impl::GetTextureWidth(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    Napi::Value NativeEngine::Impl::GetTextureHeight(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        assert(textureData->Images.size() > 0 && !textureData->Images.front()->m_cubeMap);
        return Napi::Value::From(info.Env(), textureData->Images.front()->m_width);
    }

    void NativeEngine::Impl::SetTextureSampling(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto filter = static_cast<Filter>(info[1].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTextureWrapMode(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto addressModeU = static_cast<AddressMode>(info[1].As<Napi::Number>().Uint32Value());
        const auto addressModeV = static_cast<AddressMode>(info[2].As<Napi::Number>().Uint32Value());
        const auto addressModeW = static_cast<AddressMode>(info[3].As<Napi::Number>().Uint32Value());

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTextureAnisotropicLevel(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        const auto value = info[1].As<Napi::Number>().Uint32Value();

        // STUB: Stub.
    }

    void NativeEngine::Impl::SetTexture(const Napi::CallbackInfo& info)
    {
        const auto uniformData = info[0].As<Napi::External<UniformInfo>>().Data();
        const auto textureData = info[1].As<Napi::External<TextureData>>().Data();

        bgfx::setTexture(uniformData->Stage, uniformData->Handle, textureData->Texture);
    }

    void NativeEngine::Impl::DeleteTexture(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        delete textureData;
    }

    Napi::Value NativeEngine::Impl::CreateFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto textureData = info[0].As<Napi::External<TextureData>>().Data();
        uint16_t width = static_cast<uint16_t>(info[1].As<Napi::Number>().Uint32Value());
        uint16_t height = static_cast<uint16_t>(info[2].As<Napi::Number>().Uint32Value());
        bgfx::TextureFormat::Enum format = static_cast<bgfx::TextureFormat::Enum>(info[3].As<Napi::Number>().Uint32Value());
        int samplingMode = info[4].As<Napi::Number>().Uint32Value();
        bool generateStencilBuffer = info[5].As<Napi::Boolean>();
        bool generateDepth = info[6].As<Napi::Boolean>();
        bool generateMipMaps = info[7].As<Napi::Boolean>();

        bgfx::FrameBufferHandle frameBufferHandle{};
        if (generateStencilBuffer && !generateDepth)
        {
            throw std::exception{ /* Does this case even make any sense? */ };
        }
        else if (!generateStencilBuffer && !generateDepth)
        {
            frameBufferHandle = bgfx::createFrameBuffer(width, height, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT);
        }
        else
        {
            auto depthStencilFormat = bgfx::TextureFormat::D32;
            if (generateStencilBuffer)
            {
                depthStencilFormat = bgfx::TextureFormat::D24S8;
            }

            assert(bgfx::isTextureValid(0, false, 1, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT));
            assert(bgfx::isTextureValid(0, false, 1, depthStencilFormat, BGFX_TEXTURE_RT));

            std::array<bgfx::TextureHandle, 2> textures
            {
                bgfx::createTexture2D(width, height, generateMipMaps, 1, TEXTURE_FORMAT[format], BGFX_TEXTURE_RT),
                bgfx::createTexture2D(width, height, generateMipMaps, 1, depthStencilFormat, BGFX_TEXTURE_RT)
            };
            std::array<bgfx::Attachment, textures.size()> attachments{};
            for (int idx = 0; idx < attachments.size(); ++idx)
            {
                attachments[idx].init(textures[idx]);
            }
            frameBufferHandle = bgfx::createFrameBuffer(static_cast<uint8_t>(attachments.size()), attachments.data(), true);
        }

        textureData->Texture = bgfx::getTexture(frameBufferHandle);

        return Napi::External<FrameBufferData>::New(info.Env(), m_frameBufferManager.CreateNew(frameBufferHandle, width, height));
    }

    void NativeEngine::Impl::BindFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto frameBufferData = info[0].As<Napi::External<FrameBufferData>>().Data();
        m_frameBufferManager.Bind(frameBufferData);
    }

    void NativeEngine::Impl::UnbindFrameBuffer(const Napi::CallbackInfo& info)
    {
        const auto frameBufferData = info[0].As<Napi::External<FrameBufferData>>().Data();
        m_frameBufferManager.Unbind(frameBufferData);
    }

    void NativeEngine::Impl::DrawIndexed(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // TODO: handle viewport

        for (const auto& it : m_currentProgram->Uniforms)
        {
            const ProgramData::UniformValue& value = it.second;
            bgfx::setUniform({ it.first }, value.Data.data(), value.ElementLength);
        }

        bgfx::submit(m_frameBufferManager.IsFrameBufferBound() ? m_frameBufferManager.GetBound().ViewId : 0, m_currentProgram->Program, 0, true);
    }

    void NativeEngine::Impl::Draw(const Napi::CallbackInfo& info)
    {
        const auto fillMode = info[0].As<Napi::Number>().Int32Value();
        const auto elementStart = info[1].As<Napi::Number>().Int32Value();
        const auto elementCount = info[2].As<Napi::Number>().Int32Value();

        // STUB: Stub.
        // bgfx::submit(), right?  Which means we have to preserve here the state of
        // which program is being worked on.
    }

    void NativeEngine::Impl::Clear(const Napi::CallbackInfo& info)
    {
        if (m_frameBufferManager.IsFrameBufferBound())
        {
            m_frameBufferManager.GetBound().ViewClearState.Update(info);
        }
        else
        {
            m_viewClearState.Update(info);
        }
    }

    Napi::Value NativeEngine::Impl::GetRenderWidth(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Width);
    }

    Napi::Value NativeEngine::Impl::GetRenderHeight(const Napi::CallbackInfo& info)
    {
        // TODO CHECK: Is this not just the size?  What is this?
        return Napi::Value::From(info.Env(), m_size.Height);
    }

    void NativeEngine::Impl::DispatchAnimationFrameAsync(Napi::FunctionReference callback)
    {
        // The purpose of encapsulating the callbackPtr in a std::shared_ptr is because, under the hood, the lambda is
        // put into a kind of function which requires a copy constructor for all of its captured variables.  Because
        // the Napi::FunctionReference is not copyable, this breaks when trying to capture the callback directly, so we
        // wrap it in a std::shared_ptr to allow the capture to function correctly.
        m_runtimeImpl.Execute([this, callbackPtr = std::make_shared<Napi::FunctionReference>(std::move(callback))](auto&)
        {
            //bgfx_test(static_cast<uint16_t>(m_size.Width), static_cast<uint16_t>(m_size.Height));

            callbackPtr->Call({});
            bgfx::frame();
        });
    }

    // NativeEngine exterior definitions.

    NativeEngine::NativeEngine(void* nativeWindowPtr, RuntimeImpl& runtimeImpl, uint32_t width, uint32_t height)
        : m_impl{ std::make_unique<NativeEngine::Impl>(nativeWindowPtr, runtimeImpl, width, height) }
    {
    }

    NativeEngine::~NativeEngine()
    {
    }

    void NativeEngine::Initialize(Napi::Env& env)
    {
        m_impl->Initialize(env);
    }

    void NativeEngine::UpdateSize(float width, float height)
    {
        m_impl->UpdateSize(width, height);
    }

    void NativeEngine::UpdateRenderTarget()
    {
        m_impl->UpdateRenderTarget();
    }
}

