// Small synthetic renderer regression; no game assets, OpenXR or Streamline.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>
#include "rw.h"
#include "src/d3d12/rwd3d12impl.h"
#include "src/d3d12/d3d12alphacoverage.h"

static void Require(bool ok, const char *name)
{
    if(!ok){ std::printf("FAIL %s\n", name); std::exit(1); }
    std::printf("PASS %s\n", name);
}

static std::uint64_t ImageHash(rw::Raster *raster)
{
    rw::Image *image = raster->toImage();
    Require(image && image->pixels, "color readback");
    std::uint64_t hash = 1469598103934665603ULL;
    unsigned colored = 0;
    for(int y = 0; y < image->height; ++y)
        for(int x = 0; x < image->width; ++x){
            const auto *pixel = image->pixels + y*image->stride + x*4;
            colored += pixel[0] > 64 || pixel[1] > 64 || pixel[2] > 64;
            for(int c = 0; c < 3; ++c){ hash ^= pixel[c]; hash *= 1099511628211ULL; }
        }
    image->destroy();
    Require(colored > 32, "world geometry reached render target");
    return hash;
}

static void ReferenceAlphaCoverage(const uint8_t *source, unsigned sw, unsigned sh,
    unsigned ss, uint8_t *target, unsigned tw, unsigned th, unsigned ts)
{
    const unsigned wanted = unsigned((uint64_t(rw::d3d12::countAlphaCoverage(
        source,sw,sh,ss,1.0f))*tw*th + sw*sh/2)/(sw*sh));
    if(!wanted) return;
    float low = 1.0f, high = 255.0f;
    if(rw::d3d12::countAlphaCoverage(target,tw,th,ts,low) >= wanted) high = low;
    else for(unsigned iteration = 0; iteration < 12; ++iteration){
        const float middle = (low+high)*0.5f;
        if(rw::d3d12::countAlphaCoverage(target,tw,th,ts,middle) >= wanted) high = middle;
        else low = middle;
    }
    for(unsigned y = 0; y < th; ++y) for(unsigned x = 0; x < tw; ++x){
        const unsigned scaled = unsigned(target[y*ts+x*4+3]*high+0.5f);
        target[y*ts+x*4+3] = uint8_t(scaled < 255 ? scaled : 255);
    }
}

static void TestAlphaCoverage()
{
    uint32_t seed = 0x38E0217Bu;
    const auto random = [&](){ seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    const unsigned dimensions[] = {1,2,3,7,15,16,17,31,64,129,256,512};
    unsigned cases = 0;
    for(unsigned tw : dimensions) for(unsigned th : dimensions) for(unsigned pattern = 0; pattern < 5; ++pattern){
        const unsigned sw = tw*2, sh = th*2, ss = sw*4+8, ts = tw*4+12;
        std::vector<uint8_t> source(ss*sh), target(ts*th);
        for(auto &v : source) v = uint8_t(random());
        for(auto &v : target) v = uint8_t(random());
        if(pattern < 3){
            for(unsigned y = 0; y < sh; ++y) for(unsigned x = 0; x < sw; ++x)
                source[y*ss+x*4+3] = pattern == 0 ? 0 : pattern == 1 ? 255 : uint8_t((random()&1)*255);
            for(unsigned y = 0; y < th; ++y) for(unsigned x = 0; x < tw; ++x)
                target[y*ts+x*4+3] = pattern == 0 ? 0 : pattern == 1 ? 255 : uint8_t(random()%128);
        }
        auto reference = target;
        ReferenceAlphaCoverage(source.data(),sw,sh,ss,reference.data(),tw,th,ts);
        rw::d3d12::preserveAlphaCoverage(source.data(),sw,sh,ss,target.data(),tw,th,ts);
        if(target != reference) Require(false,"mip alpha equivalence");
        ++cases;
    }
    std::printf("MIP_COMPLETE cases=%u failures=0\n",cases);
}

int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    TestAlphaCoverage();
    ID3D12Debug *debug = nullptr;
    const bool debugAvailable = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(debugAvailable){ debug->EnableDebugLayer(); debug->Release(); }
    std::printf("D3D12 debug layer=%d\n", debugAvailable);
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"ViceCityRendererProbe";
    RegisterClassW(&windowClass);
    // Deliberately omit WS_VISIBLE. This is an off-screen backend probe.
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Renderer probe", 0,
        0, 0, 128, 128, nullptr, nullptr, windowClass.hInstance, nullptr);
    Require(window != nullptr, "hidden window");
    rw::platform = rw::PLATFORM_D3D12;
    Require(rw::Engine::init() != 0, "engine init");
    rw::registerMeshPlugin();
    rw::registerNativeDataPlugin();
    rw::registerSkinPlugin();
    rw::registerHAnimPlugin();
    rw::registerMatFXPlugin();
    rw::EngineOpenParams params = {};
    params.window = window;
    Require(rw::Engine::open(&params) && rw::Engine::start(), "engine start and shader compilation");
    Require(rw::d3d12::isPresentationReady(), "D3D12 device and PSO initialization");
    std::set<UINT64> samplers;
    for(unsigned bias = 0; bias < 4; ++bias)
    for(unsigned filter = 1; filter <= 6; ++filter)
    for(unsigned u = 1; u <= 4; ++u) for(unsigned v = 1; v <= 4; ++v){
        D3D12_GPU_DESCRIPTOR_HANDLE first = {}, repeated = {};
        Require(rw::d3d12::getSamplerView(filter,u,v,&first,bias) &&
            rw::d3d12::getSamplerView(filter,u,v,&repeated,bias) &&
            first.ptr != 0 && first.ptr == repeated.ptr,
            "sampler variant allocation and reuse");
        Require(samplers.insert(first.ptr).second,"sampler variants do not alias");
    }
    Require(samplers.size() == 384,"all bounded sampler combinations fit heap");
    D3D12_GPU_DESCRIPTOR_HANDLE lastBias = {}, clampedBias = {};
    Require(rw::d3d12::getSamplerView(6,1,1,&lastBias,3) &&
        rw::d3d12::getSamplerView(6,1,1,&clampedBias,~0u) &&
        lastBias.ptr == clampedBias.ptr,"sampler bias clamps before cache access");
    auto *generated = rw::Raster::create(64,64,32,rw::Raster::TEXTURE | rw::Raster::C8888);
    Require(generated && generated->getNumLevels() == 7 &&
        rw::d3d12::rasterHasGeneratedMips(generated),"generated mip default remains enabled");
    rw::d3d12::setGenerateMipmaps(false);
    auto *withoutMips = rw::Raster::create(64,64,32,rw::Raster::TEXTURE | rw::Raster::C8888);
    auto *authoredMips = rw::Raster::create(64,64,32,
        rw::Raster::TEXTURE | rw::Raster::C8888 | rw::Raster::MIPMAP);
    Require(withoutMips && withoutMips->getNumLevels() == 1 &&
        !rw::d3d12::rasterHasGeneratedMips(withoutMips),"startup mip OFF avoids generated chain");
    Require(authoredMips && authoredMips->getNumLevels() == 7 &&
        !rw::d3d12::rasterHasGeneratedMips(authoredMips),"startup mip OFF preserves authored levels");
    Require(generated->getNumLevels() == 7,"mip setting does not mutate resident textures");
    rw::d3d12::setGenerateMipmaps(true);
    generated->destroy(); withoutMips->destroy();
    ID3D12InfoQueue *info = nullptr;
    rw::d3d12::getDevice()->QueryInterface(IID_PPV_ARGS(&info));
    auto *world = rw::World::create();
    auto *camera = rw::Camera::create();
    auto *color = rw::Raster::create(128, 128, 32, rw::Raster::CAMERATEXTURE | rw::Raster::C8888);
    auto *depth = rw::Raster::create(128, 128, 0, rw::Raster::ZBUFFER);
    Require(color && depth && world && camera, "camera resources");
    camera->frameBuffer = color; camera->zBuffer = depth;
    auto *cameraFrame = rw::Frame::create(); camera->setFrame(cameraFrame);
    camera->setNearPlane(0.1f); camera->setFarPlane(100.0f); camera->setFOV(60.0f, 1.0f);
    world->addCamera(camera);
    auto *geometry = rw::Geometry::create(3, 1,
        rw::Geometry::POSITIONS | rw::Geometry::NORMALS | rw::Geometry::PRELIT | rw::Geometry::TEXTURED);
    const rw::V3d positions[3] = { {-0.7f,-0.7f,2.0f}, {0.7f,-0.7f,2.0f}, {0.0f,0.7f,2.0f} };
    const rw::TexCoords uvs[3] = {{0,0},{4,0},{2,4}};
    for(int i = 0; i < 3; ++i){
        geometry->morphTargets[0].vertices[i] = positions[i];
        geometry->morphTargets[0].normals[i] = {0,0,-1};
        geometry->colors[i] = {200,150,100,255};
        geometry->texCoords[0][i] = uvs[i];
        geometry->triangles[0].v[i] = (rw::uint16)i;
    }
    auto *material = rw::Material::create();
    geometry->triangles[0].matId = (rw::uint16)geometry->matList.appendMaterial(material);
    material->destroy(); geometry->calculateBoundingSphere(); geometry->buildMeshes();
    auto *atomic = rw::Atomic::create(); auto *atomicFrame = rw::Frame::create();
    atomic->setFrame(atomicFrame); atomic->setGeometry(geometry, 0); geometry->destroy();
    rw::d3d12::setScreenSpaceReflectionSettings(0,0,0);
    rw::d3d12::setRainSurfaceSettings(0,0,0,100,100,0,0);
    rw::d3d12::setDynamicPointLights(nullptr,0,0);
    rw::d3d12::setRenderState(rw::CULLMODE, (void*)rw::CULLNONE);
    ID3D12Resource *motion = nullptr, *originalMotion = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE motionView = {};
    Require(!rw::d3d12::getMotionTarget(color, &motion, &motionView), "ordinary camera has no motion allocation");
    auto *leftColor = rw::Raster::create(0,0,0,rw::Raster::CAMERATEXTURE | rw::Raster::C8888 | rw::Raster::DONTALLOCATE);
    rw::Rect leftRect = {0,0,64,128}; leftColor->subRaster(color,&leftRect);
    rw::Matrix bone; bone.setIdentity();
    rw::HAnimHierarchy hierarchy = {};
    hierarchy.numNodes = 1; hierarchy.matrices = &bone;
    hierarchy.flags = rw::HAnimHierarchy::LOCALSPACEMATRICES;
    for(int skinCase = 0; skinCase < 2; ++skinCase){
    if(skinCase){
        atomic->uninstance();
        auto *skin = rwNewT(rw::Skin,1,rw::MEMDUR_EVENT | rw::ID_SKIN);
        skin->init(1,1,3); skin->numWeights = 1; skin->usedBones[0] = 0;
        std::memset(skin->indices,0,12); std::memset(skin->weights,0,12*sizeof(float));
        for(int vertex = 0; vertex < 3; ++vertex) skin->weights[vertex*4] = 1.0f;
        std::memcpy(skin->inverseMatrices,&bone,sizeof(bone));
        rw::Skin::set(atomic->geometry,skin); rw::Skin::setHierarchy(atomic,&hierarchy);
    }
    for(int stereoCase = 0; stereoCase < 2; ++stereoCase){
    camera->frameBuffer = stereoCase ? leftColor : color;
    std::uint64_t baselineHash = 0;
    const bool modes[] = {false,true,true,false,true};
    for(unsigned frame = 0; frame < sizeof(modes)/sizeof(modes[0]); ++frame){
        Require(rw::d3d12::setRasterMotionEnabled(color,modes[frame]), "set motion mode");
        rw::d3d12::beginStereoWorldFrame(); rw::d3d12::setStereoWorldEye(0);
        rw::RGBA clear = {0,0,0,255};
        camera->clear(&clear,rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ); camera->beginUpdate();
        if(stereoCase) rw::d3d12::captureStereoWorldCamera(1,0,0);
        rw::d3d12::captureStereoWorldCamera(0,0,0);
        const bool hasMotion = rw::d3d12::getMotionTarget(color,&motion,&motionView) != 0;
        Require(hasMotion == modes[frame], "motion attachment matches selected consumer");
        if(hasMotion){
            if(originalMotion) Require(originalMotion == motion, "motion reactivation reuses allocation");
            originalMotion = motion;
        }
        rw::d3d12::setWorldRenderStage(rw::d3d12::WORLD_STAGE_ENTITIES);
        if(stereoCase) Require(rw::d3d12::beginStereoSinglePass(),"stereo single pass enabled");
        atomic->render();
        if(stereoCase) rw::d3d12::endStereoSinglePass();
        camera->endUpdate();
        const auto hash = ImageHash(color);
        if(frame == 0) baselineHash = hash;
        else Require(hash == baselineHash, "motion and no-motion color equivalence");
        std::printf("FRAME %u skin=%d stereo=%d motion=%d hash=%016llX\n",
            frame,skinCase,stereoCase,modes[frame],(unsigned long long)hash);
        Require(rw::d3d12::submitAndWaitForExternal(), "frame drain");
    }
    }
    }
    rw::d3d12::releaseRasterMotionResources(color);
    Require(!rw::d3d12::getMotionTarget(color,&motion,&motionView),"full backend shutdown releases motion");
    Require(rw::d3d12::setRasterMotionEnabled(color,true),"full backend reactivation allocates motion");
    camera->beginUpdate();
    Require(rw::d3d12::getMotionTarget(leftColor,&motion,&motionView),"subraster shares parent motion attachment");
    camera->endUpdate();
    Require(rw::d3d12::submitAndWaitForExternal(),"reactivation drain");
    auto *copy = rw::Raster::create(128,128,32,rw::Raster::CAMERATEXTURE | rw::Raster::C8888);
    Require(copy && !rw::d3d12::getMotionTarget(copy,&motion,&motionView), "SSR or HUD camera stays motion-free");
    copy->destroy();
    camera->frameBuffer = color;
    rw::d3d12::setRasterMotionEnabled(color,false);
    const auto renderSample = [&](bool captureHistory = false){
        rw::d3d12::beginStereoWorldFrame(); rw::d3d12::setStereoWorldEye(0);
        rw::RGBA clear = {0,0,0,255};
        camera->clear(&clear,rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ); camera->beginUpdate();
        rw::d3d12::captureStereoWorldCamera(0,0,0);
        rw::d3d12::setWorldRenderStage(rw::d3d12::WORLD_STAGE_ENTITIES);
        atomic->render(); camera->endUpdate();
        if(captureHistory)
            Require(rw::d3d12::captureScreenSpaceReflectionFrame(color),"seed reflection history");
        const auto hash = ImageHash(color);
        Require(rw::d3d12::submitAndWaitForExternal(),"setting frame drain");
        return hash;
    };
    camera->setFarPlane(4.0f); camera->fogPlane = 1.0f;
    const auto defaultFog = renderSample();
    rw::d3d12::setDistanceFogEnabled(false);
    const auto withoutFog = renderSample();
    Require(defaultFog != withoutFog,"distance fog OFF changes fogged world output");
    rw::d3d12::setDistanceFogEnabled(true);
    Require(defaultFog == renderSample(),"distance fog ON restores default output");
    camera->setFarPlane(100.0f); camera->fogPlane = 100.0f;

    for(int level = 0; level < authoredMips->getNumLevels(); ++level){
        auto *pixels = authoredMips->lock(level,rw::Raster::LOCKWRITE | rw::Raster::LOCKNOFETCH);
        Require(pixels != nullptr,"authored mip lock");
        for(int y = 0; y < authoredMips->height; ++y)
        for(int x = 0; x < authoredMips->width; ++x){
            auto *pixel = pixels+y*authoredMips->stride+x*4;
            pixel[0] = uint8_t(40+level*25); pixel[1] = uint8_t(240-level*20);
            pixel[2] = 60; pixel[3] = 192;
        }
        authoredMips->unlock(level);
    }
    rw::d3d12::setRasterHasAlpha(authoredMips,true);
    auto *mipTexture = rw::Texture::create(authoredMips);
    mipTexture->setFilter(rw::Texture::LINEARMIPLINEAR);
    mipTexture->setAddressU(rw::Texture::WRAP); mipTexture->setAddressV(rw::Texture::WRAP);
    material->setTexture(mipTexture); mipTexture->destroy();
    rw::d3d12::setRenderState(rw::ALPHATESTFUNC,(void*)rw::ALPHAGREATEREQUAL);
    rw::d3d12::setRenderState(rw::ALPHATESTREF,(void*)128);
    rw::d3d12::setRenderState(rw::GSALPHATEST,nullptr);
    const auto defaultBias = renderSample();
    std::set<std::uint64_t> biasHashes; biasHashes.insert(defaultBias);
    for(unsigned bias = 1; bias < 4; ++bias){
        rw::d3d12::setMaskedMipBias(bias);
        Require(biasHashes.insert(renderSample()).second,"masked mip half-step changes sampled output");
    }
    rw::d3d12::setVehicleAlphaPass(true);
    Require(defaultBias == renderSample(),"vehicle alpha pass excludes foliage bias");
    rw::d3d12::setVehicleAlphaPass(false);
    rw::d3d12::setMaskedMipBias(0);
    Require(defaultBias == renderSample(),"foliage bias OFF restores default output");
    rw::d3d12::setRenderState(rw::ALPHATESTFUNC,(void*)rw::ALPHAALWAYS);
    const auto unmaskedBias = renderSample();
    rw::d3d12::setMaskedMipBias(3);
    Require(unmaskedBias == renderSample(),"nonmasked material excludes foliage bias");
    rw::d3d12::setMaskedMipBias(0);
    material->setTexture(nullptr);
    atomic->uninstance();
    atomic->geometry->lock(rw::Geometry::LOCKNORMALS | rw::Geometry::LOCKPRELIGHT);
    const rw::RGBA sampleColours[] = {{32,220,32,255},{220,32,32,255},{32,32,220,255}};
    for(int i = 0; i < 3; ++i){
        atomic->geometry->colors[i] = sampleColours[i];
        atomic->geometry->morphTargets[0].normals[i] = {1,0,0};
    }
    atomic->geometry->unlock();
    rw::d3d12::setScreenSpaceReflectionSettings(0,0,0);
    rw::d3d12::setEffectsTime(0);
    const float sunDirection[] = {0,0,1}, sunColour[] = {1,0.6f,0.2f}, skyColour[] = {0.2f,0.5f,1};
    const auto ordinarySurface = renderSample();
    rw::d3d12::setIm3DWater(true);
    Require(ordinarySurface == renderSample(),"water extras default OFF preserves original RGB");
    rw::d3d12::setWaterSurfaceSettings(100,100,100,0,0);
    rw::d3d12::setWaterLighting(sunDirection,sunColour,skyColour);
    Require(ordinarySurface != renderSample(),"water sky sheen changes water RGB");
    rw::d3d12::setIm3DWater(false);
    Require(ordinarySurface == renderSample(),"water sheen excludes ordinary surfaces");
    rw::d3d12::setIm3DWater(true);
    rw::d3d12::setWaterSurfaceSettings(100,100,0,100,0);
    Require(ordinarySurface != renderSample(),"water sun glint changes water RGB");
    rw::d3d12::setIm3DWater(false);
    Require(ordinarySurface == renderSample(),"water sun glint excludes ordinary surfaces");
    rw::d3d12::setIm3DWater(true);
    rw::d3d12::setWaterSurfaceSettings(100,100,0,0,0);
    Require(ordinarySurface == renderSample(),"water extras OFF restores original RGB");
    rw::d3d12::DynamicPointLight waterLight = {};
    waterLight.position[2] = 4; waterLight.radius = 10;
    waterLight.colour[0] = 0.6f; waterLight.colour[1] = 0.2f;
    rw::d3d12::setDynamicPointLights(&waterLight,1,0);
    const auto waterWithoutSparks = renderSample();
    rw::d3d12::setWaterSurfaceSettings(100,100,0,0,100);
    Require(waterWithoutSparks != renderSample(),"water light sparks add optional specular");
    rw::d3d12::setIm3DWater(false);
    Require(waterWithoutSparks == renderSample(),"water light sparks exclude ordinary surfaces");
    rw::d3d12::setDynamicPointLights(nullptr,0,0);
    rw::d3d12::setWaterSurfaceSettings(100,100,0,0,0);
    rw::d3d12::setIm3DWater(false);
    camera->frameBuffer = leftColor;
    rw::d3d12::setScreenSpaceReflectionSettings(100,100,100);
    renderSample(true);
    rw::d3d12::setIm3DWater(true);
    const auto waterReflection = renderSample();
    rw::d3d12::setWaterSurfaceSettings(100,0,0,0,0);
    Require(waterReflection != renderSample(),"water distortion independently changes reflection");
    rw::d3d12::setWaterSurfaceSettings(100,100,0,0,0);
    Require(waterReflection == renderSample(),"water distortion default restores reflection");
    rw::d3d12::setEffectsTime(5);
    const auto movingWaves = renderSample();
    Require(waterReflection != movingWaves,"explicit game time advances waves");
    rw::d3d12::setWaterSurfaceSettings(0,100,0,0,0);
    Require(waterReflection == renderSample(),"zero water speed freezes at initial phase");
    rw::d3d12::setWaterSurfaceSettings(100,100,0,0,0);
    rw::d3d12::setEffectsTime(0);
    rw::d3d12::setIm3DWater(false);
    rw::MatFX::setEffects(material,rw::MatFX::ENVMAP);
    rw::MatFX::get(material)->setEnvCoefficient(1.0f);
    const auto carReflection = renderSample();
    rw::d3d12::setCarReflectionSsrParams(0,0);
    const auto noCarReflection = renderSample();
    Require(carReflection != noCarReflection,"car SSR strength zero skips live contribution");
    rw::d3d12::setCarReflectionSsrParams(100,1);
    Require(noCarReflection == renderSample(),"car SSR distance excludes distant surface");
    rw::d3d12::setCarReflectionSsrParams(100,0);
    Require(carReflection == renderSample(),"car SSR unlimited restores original RGB");
    rw::d3d12::setScreenSpaceReflectionSettings(100,0,100);
    rw::d3d12::setCarReflectionSsrParams(0,0);
    Require(rw::d3d12::captureScreenSpaceReflectionFrame(nullptr),"car SSR OFF permits no-source fast path");
    rw::d3d12::setScreenSpaceReflectionSettings(0,0,100);
    atomic->destroy(); atomicFrame->destroy();
    world->removeCamera(camera); camera->setFrame(nullptr);
    camera->frameBuffer = camera->zBuffer = nullptr;
    camera->destroy(); cameraFrame->destroy(); leftColor->destroy(); color->destroy(); depth->destroy(); world->destroy();
    Require(rw::d3d12::waitForGpu(), "cleanup drain");
    unsigned errors = 0;
    if(info){
        for(UINT64 index = 0; index < info->GetNumStoredMessages(); ++index){
            SIZE_T bytes = 0; info->GetMessage(index,nullptr,&bytes);
            std::vector<unsigned char> storage(bytes);
            auto *message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(info->GetMessage(index,message,&bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING){
                std::printf("D3D12 %u: %s\n", unsigned(message->Severity), message->pDescription);
                if(message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++errors;
            }
        }
        info->Release();
    }
    rw::Engine::stop(); rw::Engine::close(); rw::Engine::term();
    DestroyWindow(window);
    std::printf("RENDERER_COMPLETE failures=%u\n",errors);
    return errors ? 1 : 0;
}
