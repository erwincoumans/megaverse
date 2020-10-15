#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Optional.h>

#include <Magnum/GL/Buffer.h>
#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/ImageView.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Primitives/Cube.h>
#include <Magnum/Primitives/Capsule.h>
#include <Magnum/Primitives/Icosphere.h>
#include <Magnum/Shaders/Phong.h>
#include <Magnum/Shaders/Flat.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/SceneGraph/MatrixTransformation3D.h>
#include <Magnum/SceneGraph/Camera.h>
#include <Magnum/SceneGraph/Drawable.h>
#include <Magnum/MeshTools/Compile.h>
#include <Magnum/GL/RenderbufferFormat.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/GL/Renderbuffer.h>
#include <Magnum/GL/Context.h>
#include <Magnum/Platform/WindowlessEglApplication.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/BulletIntegration/DebugDraw.h>

#include <util/tiny_logger.hpp>

#include <magnum_rendering/windowless_context.hpp>
#include <magnum_rendering/magnum_env_renderer.hpp>



using namespace Magnum;
using namespace Magnum::Math::Literals;

using namespace VoxelWorld;


struct InstanceData {
    Magnum::Matrix4 transformationMatrix;
    Magnum::Matrix3x3 normalMatrix;
    Magnum::Color3 color;
};


class CustomDrawable : public SceneGraph::Drawable3D
{
public:
    explicit CustomDrawable(
        SceneGraph::AbstractObject3D &parentObject,
        Containers::Array<InstanceData> &instanceData,
        const Color3 &color,
        SceneGraph::DrawableGroup3D &drawables
    )
    : SceneGraph::Drawable3D{parentObject, &drawables}, _instanceData(instanceData), _color{color}
    {}

private:
    void draw(const Matrix4& t, SceneGraph::Camera3D&) override {
        arrayAppend(_instanceData, Containers::InPlaceInit, t, t.normalMatrix(), _color);
    }

    Containers::Array<InstanceData>& _instanceData;
    Color3 _color;
};


#ifdef UNUSED
class SimpleDrawable3D : public Object3D, public SceneGraph::Drawable3D
{
public:
    explicit SimpleDrawable3D(SceneGraph::DrawableGroup3D &drawables,
                              Shaders::Phong & shader, GL::Mesh& mesh, const Color3 &color, Object3D *parentObject)
        : Object3D{parentObject}
        , SceneGraph::Drawable3D{*this, &drawables}
        , _shader(shader), _mesh(mesh), _color(color)
    {}

    void draw(const Matrix4& transformationMatrix, SceneGraph::Camera3D& camera) override
    {
        _shader.setTransformationMatrix(transformationMatrix)
            .setNormalMatrix(transformationMatrix.normalMatrix())
            .setProjectionMatrix(camera.projectionMatrix())
            .setAmbientColor(0.4 * _color)
            .setDiffuseColor(_color)
            .setShininess(150)
                /* relative to the camera */
            .setLightPosition({13.0f, 2.0f, 5.0f})
            .setLightColor(0xaaaaaa_rgbf)
            .draw(_mesh);
    }

private:
    Shaders::Phong& _shader;
    GL::Mesh& _mesh;
    Color3 _color;
};
#endif


struct MagnumEnvRenderer::Impl
{
public:
    explicit Impl(Envs &envs, int w, int h, bool withDebugDraw = false, RenderingContext *ctx = nullptr);

    ~Impl();

    RenderingContext * initContext(RenderingContext *ctx);

    /**
     * Reset the state of the renderer between episodes.
     * @param env
     */
    void reset(Env &env, int envIndex);

    void preDraw(Env &env, int envIndex);
    void draw(Envs &envs);
    void drawAgent(Env &env, int envIndex, int agentIdx, bool readToBuffer);
    void postDraw(Env &env, int envIndex);

    const uint8_t * getObservation(int envIdx, int agentIdx) const;

    GL::Framebuffer * getFramebuffer() { return &framebuffer; }

    void toggleOverviewMode() { overviewMode = !overviewMode; }

public:
    std::unique_ptr<WindowlessContext> windowlessContextPtr{nullptr};
    RenderingContext *ctx = nullptr;

    Vector2i framebufferSize;

    std::vector<SceneGraph::DrawableGroup3D> envDrawables;

    GL::Buffer boxInstanceBuffer{NoCreate}, capsuleInstanceBuffer{NoCreate}, sphereInstanceBuffer{NoCreate};
    Containers::Array<InstanceData> boxInstanceData, capsuleInstanceData, sphereInstanceData;

    Shaders::Phong shader{NoCreate};
    Shaders::Phong shaderInstanced{NoCreate};

    GL::Framebuffer framebuffer;
    GL::Renderbuffer colorBuffer, depthBuffer;

    GL::Mesh boxMesh, capsuleMesh, sphereMesh;

    std::vector<std::vector<Containers::Array<uint8_t>>> agentFrames;
    std::vector<std::vector<std::unique_ptr<MutableImageView2D>>> agentImageViews;

    bool withDebugDraw;
    BulletIntegration::DebugDraw debugDraw{NoCreate};

    bool withOverviewCamera = false;
    bool overviewMode = false;
    Magnum::SceneGraph::Camera3D *overviewCamera;
    Object3D *overviewCameraObject;
};


MagnumEnvRenderer::Impl::Impl(Envs &envs, int w, int h, bool withDebugDraw, RenderingContext *ctx)
    : ctx{initContext(ctx)}
    , framebufferSize{w, h}
    , framebuffer{Magnum::Range2Di{{}, framebufferSize}}
    , withDebugDraw{withDebugDraw}
{
    assert(!envs.empty());

    MAGNUM_ASSERT_GL_VERSION_SUPPORTED(GL::Version::GL330);

    GL::Renderer::enable(GL::Renderer::Feature::DepthTest);
    GL::Renderer::enable(GL::Renderer::Feature::FaceCulling);

    colorBuffer.setStorage(GL::RenderbufferFormat::SRGB8Alpha8, framebufferSize);
    depthBuffer.setStorage(GL::RenderbufferFormat::DepthComponent24, framebufferSize);

    framebuffer.attachRenderbuffer(GL::Framebuffer::ColorAttachment{0}, colorBuffer);
    framebuffer.attachRenderbuffer(GL::Framebuffer::BufferAttachment::Depth, depthBuffer);
    framebuffer.mapForDraw({{Shaders::Phong::ColorOutput, GL::Framebuffer::ColorAttachment{0}}});

    framebuffer.clearColor(0, Color3{0.125f}).clearDepth(1.0).bind();

    CORRADE_INTERNAL_ASSERT(framebuffer.checkStatus(GL::FramebufferTarget::Draw) == GL::Framebuffer::Status::Complete);

    TLOG(INFO) << "Creating Magnum env renderer " << w << " " << h << " " << envs.size();

    for (const auto &e : envs) {
        std::vector<Containers::Array<uint8_t>> envAgentFrames;
        std::vector<std::unique_ptr<MutableImageView2D>> envAgentImageViews;

        for (int i = 0; i < e->getNumAgents(); ++i) {
            envAgentFrames.emplace_back(size_t(framebufferSize.x() * framebufferSize.y() * 4));
            envAgentImageViews.emplace_back(
                std::make_unique<MutableImageView2D>(PixelFormat::RGBA8Unorm, framebufferSize, envAgentFrames[i])
            );
        }

        agentFrames.emplace_back(std::move(envAgentFrames));
        agentImageViews.emplace_back(std::move(envAgentImageViews));
    }

    shaderInstanced = Shaders::Phong{Shaders::Phong::Flag::VertexColor | Shaders::Phong::Flag::InstancedTransformation};
    shaderInstanced.setShininess(300).setLightPosition({0, 4, 2}).setLightColor(0xaaaaaa_rgbf);
    shaderInstanced.setDiffuseColor(0xbbbbbb_rgbf);
    shaderInstanced.setAmbientColor(0x555555_rgbf);

    shader = Shaders::Phong{};

    // meshes
    {
        boxMesh = MeshTools::compile(Primitives::cubeSolid());
        boxInstanceBuffer = GL::Buffer{};
        boxMesh.addVertexBufferInstanced(
            boxInstanceBuffer, 1, 0,
            Shaders::Phong::TransformationMatrix{},
            Shaders::Phong::NormalMatrix{},
            Shaders::Phong::Color3{}
        );

        capsuleMesh = MeshTools::compile(Primitives::capsule3DSolid(3, 3, 8, 1.0));
        capsuleInstanceBuffer = GL::Buffer{};
        capsuleMesh.addVertexBufferInstanced(
            capsuleInstanceBuffer, 1, 0,
            Shaders::Phong::TransformationMatrix{},
            Shaders::Phong::NormalMatrix{},
            Shaders::Phong::Color3{}
        );

        sphereMesh = MeshTools::compile(Primitives::icosphereSolid(1));
        sphereInstanceBuffer = GL::Buffer{};
        sphereMesh.addVertexBufferInstanced(
            sphereInstanceBuffer, 1, 0,
            Shaders::Phong::TransformationMatrix{},
            Shaders::Phong::NormalMatrix{},
            Shaders::Phong::Color3{}
        );
    }

    // drawables
    {
        envDrawables = std::vector<SceneGraph::DrawableGroup3D>(envs.size());
    }

    if (withDebugDraw) {
        debugDraw = BulletIntegration::DebugDraw{};
        debugDraw.setMode(BulletIntegration::DebugDraw::Mode::DrawWireframe);

        for (auto &e : envs)
            e->getPhysics().bWorld.setDebugDrawer(&debugDraw);
    }


    if (withOverviewCamera) {
        overviewCameraObject = &(envs[0]->getScene().addChild<Object3D>());
        overviewCameraObject->rotateX(-40.0_degf);
        overviewCameraObject->rotateY(225.0_degf);
        overviewCameraObject->translate(Magnum::Vector3{0.8f, 10.0f, 0.8f});

        overviewCamera = &(overviewCameraObject->addFeature<SceneGraph::Camera3D>());

        overviewCamera->setAspectRatioPolicy(SceneGraph::AspectRatioPolicy::Extend)
                      .setProjectionMatrix(Matrix4::perspectiveProjection(105.0_degf, 128.0f / 72.0f, 0.1f, 50.0f))
                      .setViewport(GL::defaultFramebuffer.viewport().size());
    }
}

MagnumEnvRenderer::Impl::~Impl()
{
    TLOG(INFO) << __PRETTY_FUNCTION__;
    ctx->makeCurrent();
}

RenderingContext * MagnumEnvRenderer::Impl::initContext(RenderingContext *ctx)
{
    if (ctx) {
        // we're given a rendering context
    } else {
        // don't have an active rendering context, let's create one
        TLOG(INFO) << windowlessContextPtr.get();
        windowlessContextPtr = std::make_unique<WindowlessContext>();
        ctx = windowlessContextPtr.get();
    }

    return ctx;
}

void MagnumEnvRenderer::Impl::reset(Env &env, int envIndex)
{
    ctx->makeCurrent();

    // reset renderer data structures
    {
        envDrawables[envIndex] = SceneGraph::DrawableGroup3D{};
        arrayResize(boxInstanceData, 0);
        arrayResize(capsuleInstanceData, 0);
        arrayResize(sphereInstanceData, 0);
    }

    // drawables
    {
        const auto &drawables = env.getDrawables();

        for (const auto &sceneObjectInfo : drawables.at(DrawableType::Box)) {
            const auto &color = sceneObjectInfo.color;
            sceneObjectInfo.objectPtr->addFeature<CustomDrawable>(boxInstanceData, color, envDrawables[envIndex]);
        }

        for (const auto &sceneObjectInfo : drawables.at(DrawableType::Capsule)) {
            const auto &color = sceneObjectInfo.color;
            sceneObjectInfo.objectPtr->addFeature<CustomDrawable>(capsuleInstanceData, color, envDrawables[envIndex]);
        }

        for (const auto &sceneObjectInfo : drawables.at(DrawableType::Sphere)) {
            const auto &color = sceneObjectInfo.color;
            sceneObjectInfo.objectPtr->addFeature<CustomDrawable>(sphereInstanceData, color, envDrawables[envIndex]);
        }
    }
}

void MagnumEnvRenderer::Impl::preDraw(Env &, int)
{
}

void MagnumEnvRenderer::Impl::drawAgent(Env &env, int envIndex, int agentIdx, bool readToBuffer)
{
    framebuffer
        .clearColor(0, Color3{0})
        .clearDepth(1.0f)
        .bind();

    arrayResize(boxInstanceData, 0);
    arrayResize(capsuleInstanceData, 0);
    arrayResize(sphereInstanceData, 0);

    auto activeCameraPtr = env.getAgents()[agentIdx]->getCamera();
    if (withOverviewCamera && overviewMode)
        activeCameraPtr = overviewCamera;

    // TODO!!! implement frustrum culling here: https://doc.magnum.graphics/magnum/classMagnum_1_1SceneGraph_1_1Drawable.html#SceneGraph-Drawable-draw-order
    activeCameraPtr->draw(envDrawables[envIndex]);

    shaderInstanced.setProjectionMatrix(activeCameraPtr->projectionMatrix());

    /* Upload instance data to the GPU (orphaning the previous buffer
       contents) and draw all cubes in one call  */
    boxInstanceBuffer.setData(boxInstanceData, GL::BufferUsage::DynamicDraw);
    boxMesh.setInstanceCount(Int(boxInstanceData.size()));
    shaderInstanced.draw(boxMesh);

    if (!capsuleInstanceData.empty()) {
        capsuleInstanceBuffer.setData(capsuleInstanceData, GL::BufferUsage::DynamicDraw);
        capsuleMesh.setInstanceCount(Int(capsuleInstanceData.size()));
        shaderInstanced.draw(capsuleMesh);
    }

    if (!sphereInstanceData.empty()) {
        sphereInstanceBuffer.setData(sphereInstanceData, GL::BufferUsage::DynamicDraw);
        sphereMesh.setInstanceCount(Int(sphereInstanceData.size()));
        shaderInstanced.draw(sphereMesh);
    }

    // Bullet debug draw
    if (withDebugDraw) {
        GL::Renderer::setDepthFunction(GL::Renderer::DepthFunction::LessOrEqual);
        debugDraw.setTransformationProjectionMatrix(activeCameraPtr->projectionMatrix()*activeCameraPtr->cameraMatrix());
        env.getPhysics().bWorld.debugDrawWorld();
        GL::Renderer::setDepthFunction(GL::Renderer::DepthFunction::Less);
    }

    if (readToBuffer) {
        framebuffer.mapForRead(GL::Framebuffer::ColorAttachment{0});
        framebuffer.read(framebuffer.viewport(), *agentImageViews[envIndex][agentIdx]);
    }
}

void MagnumEnvRenderer::Impl::postDraw(Env &env, int envIndex)
{
    const auto remainingTimeBarThickness = 2;
    const auto numPixelsInOneRow = framebuffer.viewport().size().x() * 4;
    const auto pixelsToFill = env.remainingTimeFraction() * numPixelsInOneRow;

    for (int agentIdx = 0; agentIdx < env.getNumAgents(); ++agentIdx) {
        for (int i = 0; i < remainingTimeBarThickness; ++i) {
            // viewport is flipped upside-down
            const auto rowStart = agentFrames[envIndex][agentIdx].size() - numPixelsInOneRow * (i + 1);
            memset(agentFrames[envIndex][agentIdx].data() + rowStart, 255, size_t(pixelsToFill));
        }
    }
}

void MagnumEnvRenderer::Impl::draw(Envs &envs)
{
    ctx->makeCurrent();

    for (int envIdx = 0; envIdx < int(envs.size()); ++envIdx)
        for (int agentIdx = 0; agentIdx < envs[envIdx]->getNumAgents(); ++agentIdx)
            drawAgent(*envs[envIdx], envIdx, agentIdx, true);
}

const uint8_t * MagnumEnvRenderer::Impl::getObservation(int envIdx, int agentIdx) const
{
    return agentFrames[envIdx][agentIdx].data();
}

MagnumEnvRenderer::MagnumEnvRenderer(Envs &envs, int w, int h, bool withDebugDraw, RenderingContext *ctx)
{
    pimpl = std::make_unique<Impl>(envs, w, h, withDebugDraw, ctx);
}

MagnumEnvRenderer::~MagnumEnvRenderer() = default;


void MagnumEnvRenderer::reset(Env &env, int envIdx)
{
    pimpl->reset(env, envIdx);
}

void MagnumEnvRenderer::preDraw(Env &env, int envIdx)
{
    pimpl->preDraw(env, envIdx);
}

void MagnumEnvRenderer::draw(Envs &envs)
{
    pimpl->draw(envs);
}

void MagnumEnvRenderer::postDraw(Env &env, int envIdx)
{
    pimpl->postDraw(env, envIdx);
}

void MagnumEnvRenderer::drawAgent(Env &env, int envIndex, int agentIndex, bool readToBuffer)
{
    pimpl->drawAgent(env, envIndex, agentIndex, readToBuffer);
}

const uint8_t * MagnumEnvRenderer::getObservation(int envIdx, int agentIdx) const
{
    return pimpl->getObservation(envIdx, agentIdx);
}

Magnum::GL::Framebuffer *MagnumEnvRenderer::getFramebuffer()
{
    return pimpl->getFramebuffer();
}

void MagnumEnvRenderer::toggleOverviewMode()
{
    pimpl->toggleOverviewMode();
}
