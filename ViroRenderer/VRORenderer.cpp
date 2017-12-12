//
//  VRORenderer.cpp
//  ViroRenderer
//
//  Created by Raj Advani on 4/5/16.
//  Copyright © 2016 Viro Media. All rights reserved.
//

#include "VRORenderer.h"
#include "VROTime.h"
#include "VROEye.h"
#include "VROTransaction.h"
#include "VROAllocationTracker.h"
#include "VROScene.h"
#include "VROLog.h"
#include "VRONodeCamera.h"
#include "VROTransaction.h"
#include "VROReticle.h"
#include "VRORenderDelegateInternal.h"
#include "VROFrameSynchronizerInternal.h"
#include "VROImageUtil.h"
#include "VRORenderContext.h"
#include "VROCamera.h"
#include "VROFrameTimer.h"
#include "VROFrameScheduler.h"
#include "VROChoreographer.h"
#include "VROPencil.h"
#include "VROPortalTreeRenderPass.h"
#include "VRORenderMetadata.h"
#include "VROToneMappingRenderPass.h"
#include "VRODebugHUD.h"

// Target frames-per-second. Eventually this will be platform dependent,
// but for now all of our platforms target 60.
static const double kFPSTarget = 60;

// The FOV we use for the smaller dimension of the viewport, when in
// mono-rendering mode. This is similar to Hor+ scaling, in that one
// dimension is fixed, and other is dependent on the viewport. Note
// this value is degrees from edge to edge of the frustum.
static const double kFovMonoHorizontal = 90;

#pragma mark - Initialization

VRORenderer::VRORenderer(std::shared_ptr<VROInputControllerBase> inputController) :
    _rendererInitialized(false),
    _frameSynchronizer(std::make_shared<VROFrameSynchronizerInternal>()),
    _inputController(inputController),
    _fpsTickIndex(0),
    _fpsTickSum(0) {
    _hasIncomingSceneTransition = false;
    _debugHUD = std::unique_ptr<VRODebugHUD>(new VRODebugHUD());
    _frameScheduler = std::make_shared<VROFrameScheduler>();
    _mpfTarget = 1000.0 / kFPSTarget;

    _context = std::make_shared<VRORenderContext>(_frameSynchronizer, _frameScheduler);
    _context->setPencil(std::make_shared<VROPencil>());
    memset(_fpsTickArray, 0x0, sizeof(_fpsTickArray));
}

VRORenderer::~VRORenderer() {

}

void VRORenderer::initRenderer(std::shared_ptr<VRODriver> driver) {
    initBlankTexture(*_context);
    driver->readDisplayFramebuffer();

    std::shared_ptr<VRORenderDelegateInternal> delegate = _delegate.lock();
    if (delegate) {
        delegate->setupRendererWithDriver(driver);
    }
    _debugHUD->initRenderer(driver);
    
    _choreographer = std::make_shared<VROChoreographer>(driver);
    _choreographer->setBaseRenderPass(std::make_shared<VROPortalTreeRenderPass>());
}

void VRORenderer::setDelegate(std::shared_ptr<VRORenderDelegateInternal> delegate) {
    _delegate = delegate;
}

void VRORenderer::setDebugHUDEnabled(bool enabled) {
    _debugHUD->setEnabled(enabled);
}

const std::shared_ptr<VROChoreographer> VRORenderer::getChoreographer() const {
    return _choreographer;
}

#pragma mark - FPS Computation

void VRORenderer::updateFPS(uint64_t newTick) {
    // Simple moving average: subtract value falling off, and add new value
    
    _fpsTickSum -= _fpsTickArray[_fpsTickIndex];
    _fpsTickSum += newTick;
    _fpsTickArray[_fpsTickIndex] = newTick;
    
    if (++_fpsTickIndex == kFPSMaxSamples) {
        _fpsTickIndex = 0;
    }
}

double VRORenderer::getFPS() const {
    double averageNanos = ((double) _fpsTickSum) / kFPSMaxSamples;
    return 1.0 / (averageNanos / (double) 1e9);
}

#pragma mark - Viewport and FOV

VROFieldOfView VRORenderer::computeMonoFOV(int viewportWidth, int viewportHeight) {
    return VRORenderer::computeFOV(kFovMonoHorizontal, viewportWidth, viewportHeight);
}

VROFieldOfView VRORenderer::computeFOV(float horizontalFOVDegrees, int viewportWidth, int viewportHeight) {
    if (viewportWidth < viewportHeight) {
        float fovX = horizontalFOVDegrees;
        float fovY = toDegrees(2 * atan(tan(toRadians(fovX / 2.0)) * viewportHeight / viewportWidth));
        
        return { fovX / 2.0f, fovX / 2.0f, fovY / 2.0f, fovY / 2.0f };
    }
    else {
        float fovY = horizontalFOVDegrees;
        float fovX = toDegrees(2 * atan(tan(toRadians(fovY / 2.0)) * viewportWidth / viewportHeight));
        
        return { fovX / 2.0f, fovX / 2.0f, fovY / 2.0f, fovY / 2.0f };
    }
}

float VRORenderer::getFarClippingPlane() const {
    if (_sceneController) {
        return std::max(kZFar, _sceneController->getScene()->getDistanceOfFurthestObjectFromCamera() * kZFarMultiplier);
    }
    else {
        return kZFar;
    }
}

#pragma mark - Camera and Visibility

void VRORenderer::setPointOfView(std::shared_ptr<VRONode> node) {
    _pointOfView = node;
}

VROMatrix4f VRORenderer::getLookAtMatrix() const {
    return _context->getCamera().getLookAtMatrix();
}

VROCamera VRORenderer::updateCamera(const VROViewport &viewport, const VROFieldOfView &fov,
                                    const VROMatrix4f &headRotation, const VROMatrix4f &projection) {
    
    VROCamera camera;
    camera.setHeadRotation(headRotation);
    camera.setViewport(viewport);
    camera.setFOV(fov);
    camera.setProjection(projection);
    
    // Make a default camera if no point of view is set
    if (!_pointOfView) {
        camera.setPosition({0, 0, 0 });
        camera.setBaseRotation({});
    }
    else {
        // If no node camera is set, just use the point of view node's position and
        // rotation, with standard rotation type
        if (!_pointOfView->getCamera()) {
            camera.setPosition(_pointOfView->getPosition());
            camera.setBaseRotation(_pointOfView->getRotation().getMatrix());
        }
        
        // Otherwise our camera is fully specified
        else {
            const std::shared_ptr<VRONodeCamera> &nodeCamera = _pointOfView->getCamera();
            camera.setBaseRotation(_pointOfView->getRotation().getMatrix().multiply(nodeCamera->getBaseRotation().getMatrix()));
            
            if (nodeCamera->getRotationType() == VROCameraRotationType::Standard) {
                camera.setPosition(_pointOfView->getPosition() + nodeCamera->getPosition());
            }
            else { // Orbit
                VROVector3f pos = _pointOfView->getPosition() + nodeCamera->getPosition();
                VROVector3f focal = _pointOfView->getPosition() + nodeCamera->getOrbitFocalPoint();
                
                VROVector3f v = focal.subtract(pos);
                VROVector3f ray = v.normalize();
                
                // Set the orbit position by pushing out the camera at an angle
                // defined by the current head rotation
                VROVector3f orbitedRay = headRotation.multiply(v.normalize());
                camera.setPosition(focal - orbitedRay.scale(v.magnitude()));
                
                // Set the orbit rotation. This is the current head rotation plus
                // the rotation required to get from kBaseForward to the forward
                // vector defined by the camera's position and focal point
                VROQuaternion rotation = VROQuaternion::rotationFromTo(ray, kBaseForward);
                camera.setHeadRotation(rotation.getMatrix().invert().multiply(headRotation));
            }
        }
    }

    camera.computeLookAtMatrix();
    camera.computeFrustum();

    _lastComputedCameraPosition.store(camera.getPosition());
    _lastComputedCameraRotation.store(camera.getRotation().toEuler());
    _lastComputedCameraForward.store(camera.getForward());

    if (_cameraDelegate) {
        _cameraDelegate->onCameraTransformationUpdate(
                camera.getPosition(),
                camera.getRotation().toEuler(),
                camera.getForward());
    }

    return camera;
}

#pragma mark - Rendering

void VRORenderer::prepareFrame(int frame, VROViewport viewport, VROFieldOfView fov,
                               VROMatrix4f headRotation, VROMatrix4f projection, std::shared_ptr<VRODriver> driver) {

    pglpush("Viro Start Frame %d", frame);
    if (!_rendererInitialized) {
        initRenderer(driver);
      
        _rendererInitialized = true;
        _nanosecondsLastFrame = VRONanoTime();
    }
    else {
        uint64_t nanosecondsThisFrame = VRONanoTime();
        uint64_t tick = nanosecondsThisFrame - _nanosecondsLastFrame;
        _nanosecondsLastFrame = nanosecondsThisFrame;
        
        updateFPS(tick);
    }
    
    _frameStartTime = VROTimeCurrentMillis();
    VROTransaction::update();

    _context->setFrame(frame);
    _context->setFPS(getFPS());
    notifyFrameStart();

    VROCamera camera = updateCamera(viewport, fov, headRotation, projection);
    _context->setPreviousCamera(_context->getCamera());
    _context->setCamera(camera);

    /*
     Enclosure matrix is used for rendering objects that follow the camera, such
     as skyboxes. To get them to follow the camera, we do not include the
     camera's translation component in the view matrix.
     */
    VROMatrix4f enclosureMatrix = VROMathComputeLookAtMatrix({ 0, 0, 0 }, camera.getForward(), camera.getUp());
    _context->setEnclosureViewMatrix(enclosureMatrix);
    
    /*
     Orthographic matrix is used for objects that are rendered in screen coordinates. 
     We use zero for the NCP since typically orthographic objects will have no Z
     coordinate (e.g., Z=0).
     */
    _context->setOrthographicMatrix(viewport.getOrthographicProjection(0, kZFar));
    _context->getPencil()->clear();
    
    _renderMetadata = std::make_shared<VRORenderMetadata>();

    const VRORenderContext &context = *_context.get();
    if (_sceneController) {
        if (_outgoingSceneController) {
            std::shared_ptr<VROScene> outgoingScene = _outgoingSceneController->getScene();
            outgoingScene->computeTransforms(context);
            outgoingScene->computePhysics(context);
            outgoingScene->applyConstraints(context);
            outgoingScene->updateParticles(context);
            outgoingScene->updateVisibility(context);
            outgoingScene->updateSortKeys(_renderMetadata, context, driver);
        }
        _inputController->onProcess(camera);
        std::shared_ptr<VROScene> scene = _sceneController->getScene();
        scene->computeTransforms(context);
        scene->computePhysics(context);
        scene->applyConstraints(context);
        scene->updateParticles(context);
        scene->updateVisibility(context);
        scene->updateSortKeys(_renderMetadata, context, driver);
        updateSceneEffects(driver, scene);
    }

    driver->willRenderFrame(context);
    _debugHUD->prepare(context);
    pglpop();
}

void VRORenderer::renderEye(VROEyeType eye, VROMatrix4f eyeView, VROMatrix4f eyeProjection,
                            VROViewport viewport, std::shared_ptr<VRODriver> driver) {
    pglpush("Viro Render Eye [%s]", VROEye::toString(eye).c_str());
    _choreographer->setViewport(viewport, driver);
    
    std::shared_ptr<VRORenderDelegateInternal> delegate = _delegate.lock();
    _context->setViewMatrix(eyeView);
    _context->setProjectionMatrix(eyeProjection);
    _context->setEyeType(eye);
    _context->setZNear(kZNear);
    _context->setZFar(getFarClippingPlane());
    _context->setInputController(_inputController);

    if (_sceneController) {
       if (_outgoingSceneController && _outgoingSceneController->hasActiveTransitionAnimation()) {
            _choreographer->render(eye, _sceneController->getScene(), _outgoingSceneController->getScene(),
                                   _renderMetadata, _context.get(), driver);
        }
        else {
            _choreographer->render(eye, _sceneController->getScene(), nullptr,
                                   _renderMetadata, _context.get(), driver);
        }
    }

    // This unbinds the last shader to even out our pglpush and pops
    driver->unbindShader();
    pglpop();
}

void VRORenderer::renderHUD(VROEyeType eye, VROMatrix4f eyeFromHeadMatrix, VROMatrix4f eyeProjection,
                            std::shared_ptr<VRODriver> driver) {
    pglpush("Viro Render HUD [%s]", VROEye::toString(eye).c_str());

    /*
     When rendering the HUD we want the rendered elements to 'follow' the headset;
     in other words, we want the eyeView matrix to be identity. However, we *do*
     want elements to simulate depth so that, for example, the reticle can appear
     at the depth of the object above which it's hovering (thereby reducing eyestrain).
     Because of this we need to maintain the eye interpupillary distance transform
     (the eyeFromHeadMatrix), so we simply set our view matrix to the eyeFromHeadMatrix.
     */
    _context->setViewMatrix(eyeFromHeadMatrix);
    _context->setProjectionMatrix(eyeProjection);
    _context->setEyeType(eye);
    
    std::shared_ptr<VROReticle> reticle = _inputController->getPresenter()->getReticle();
    if (reticle) {
        reticle->renderEye(eye, *_context.get(), driver);
    }
    _debugHUD->renderEye(eye, *_context.get(), driver);

    // This unbinds the last shader to even out our pglpush and pops
    driver->unbindShader();
    pglpop();
}

void VRORenderer::endFrame(std::shared_ptr<VRODriver> driver) {
    pglpush("Viro End Frame");

    if (_outgoingSceneController && !_outgoingSceneController->hasActiveTransitionAnimation()) {
        _outgoingSceneController->onSceneDidDisappear(_context.get(), driver);
        _outgoingSceneController = nullptr;
    }

    if (_hasIncomingSceneTransition && !_sceneController->hasActiveTransitionAnimation()){
        _sceneController->onSceneDidAppear(_context.get(), driver);
        _hasIncomingSceneTransition = false;
    }

    notifyFrameEnd();
     _frameEndTime = VROTimeCurrentMillis();
    double timeForProcessing = _mpfTarget - (_frameEndTime - _frameStartTime);
    
    VROFrameTimer timer(VROFrameType::Normal, timeForProcessing, _frameEndTime);
    _frameScheduler->processTasks(timer);
    
    driver->didRenderFrame(timer, *_context.get());
    pglpop();
}

#pragma mark - Scene Loading

void VRORenderer::setSceneController(std::shared_ptr<VROSceneController> sceneController,
                                     std::shared_ptr<VRODriver> driver) {
    std::shared_ptr<VROSceneController> outgoingSceneController = _sceneController;
    // Detach the inputcontroller before performing scene transitions
    if (_outgoingSceneController) {
        _outgoingSceneController->getScene()->detachInputController(_inputController);
    }
  
    // The order here is intentional, we want to let the current scene knows it's about to disappear
    // before we let the new scene know it will appear (for tear down and set up order)
    if (outgoingSceneController) {
        outgoingSceneController->onSceneWillDisappear(_context.get(), driver);
    }
    sceneController->getScene()->attachInputController(_inputController);
    sceneController->onSceneWillAppear(_context.get(), driver);

    _sceneController = sceneController;

    if (outgoingSceneController) {
        outgoingSceneController->onSceneDidDisappear(_context.get(), driver);
    }
    sceneController->onSceneDidAppear(_context.get(), driver);
}

void VRORenderer::setSceneController(std::shared_ptr<VROSceneController> sceneController, float seconds,
                                     VROTimingFunctionType timingFunctionType,
                                     std::shared_ptr<VRODriver> driver) {
    passert (sceneController != nullptr);

    _outgoingSceneController = _sceneController;
    _sceneController = sceneController;

    // Detach the inputcontroller before performing scene transitions
    if (_outgoingSceneController) {
        _outgoingSceneController->getScene()->detachInputController(_inputController);
    }

    _sceneController->getScene()->attachInputController(_inputController);
    _sceneController->onSceneWillAppear(_context.get(), driver);
    if (_outgoingSceneController) {
        _outgoingSceneController->onSceneWillDisappear(_context.get(), driver);
    }

    _hasIncomingSceneTransition = true;
    _sceneController->startIncomingTransition(seconds, timingFunctionType, _context.get());
    if (_outgoingSceneController) {
        _outgoingSceneController->startOutgoingTransition(seconds, timingFunctionType, _context.get());
    }
}

void VRORenderer::updateSceneEffects(std::shared_ptr<VRODriver> driver, std::shared_ptr<VROScene> scene) {
    if (scene->isPostProcessingEffectsUpdated()) {
        std::vector<std::string> effects = scene->getPostProcessingEffects();
        std::shared_ptr<VROPostProcessEffectFactory> postProcess = _choreographer->getPostProcessEffectFactory();
        postProcess->clearAllEffects();
        
        for (std::string &strEffect : effects){
            VROPostProcessEffect effect = postProcess->getEffectForString(strEffect);
            if (effect != VROPostProcessEffect::None) {
                postProcess->enableEffect(effect, driver);
            }
        }
        scene->setPostProcessingEffectsUpdated(false);
    }
    
    if (driver->getColorRenderingMode() != VROColorRenderingMode::NonLinear && scene->isToneMappingUpdated()) {
        std::shared_ptr<VROToneMappingRenderPass> toneMapping = _choreographer->getToneMapping();
        if (scene->isToneMappingEnabled()) {
            toneMapping->setMethod(scene->getToneMappingMethod());
            toneMapping->setExposure(scene->getToneMappingExposure());
            toneMapping->setWhitePoint(scene->getToneMappingWhitePoint());
        }
        else {
            toneMapping->setMethod(VROToneMappingMethod::Disabled);
        }
        scene->setToneMappingUpdated(false);
    }
}

#pragma mark - Frame Listeners

void VRORenderer::notifyFrameStart() {
    ((VROFrameSynchronizerInternal *)_frameSynchronizer.get())->
            notifyFrameStart(*_context.get());
}

void VRORenderer::notifyFrameEnd() {
    ((VROFrameSynchronizerInternal *)_frameSynchronizer.get())->notifyFrameEnd(*_context.get());

}

#pragma mark - VR Framework Specific

void VRORenderer::requestExitVR() {
    std::shared_ptr<VRORenderDelegateInternal> delegate = _delegate.lock();
    if (delegate) {
        delegate->userDidRequestExitVR();
    }
}
