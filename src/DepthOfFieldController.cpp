///////////////////////////////////////////////////////////////////////
//
// Part of IGCS Connector, an add on for Reshade 5+ which allows you
// to connect IGCS built camera tools with reshade to exchange data and control
// from Reshade.
// 
// (c) Frans 'Otis_Inf' Bouma.
//
// All rights reserved.
// https://github.com/FransBouma/IgcsConnector
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
/////////////////////////////////////////////////////////////////////////
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include "stdafx.h"
#include "DepthOfFieldController.h"

#include "OverlayControl.h"
#include "Utils.h"
#include <random>
#include <algorithm>
#include "CDataFile.h"

DepthOfFieldController::DepthOfFieldController(CameraToolsConnector& connector) : _cameraToolsConnector(connector), _state(DepthOfFieldControllerState::Off), _quality(4), _numberOfPointsInnermostRing(3)
{
}


void DepthOfFieldController::setMaxBokehSize(reshade::api::effect_runtime* runtime, float newValue)
{
	if(DepthOfFieldControllerState::Setup != _state || newValue <=0.0f)
	{
		// not in setup or value is out of range
		return;
	}

	const float oldValue = _maxBokehSize;
	_maxBokehSize = newValue;
	// recalculate focus x/y
	if(oldValue > 0.0f)
	{
		const float ratio = _maxBokehSize / oldValue;
		_focusDelta *= ratio;
	}
	calculateShapePoints();

	// we have to move the camera over the new distance. We move relative to the start position.
	_cameraToolsConnector.moveCameraMultishot(_maxBokehSize, 0.0f, 0.0f, true);
	// the value is passed to the shader next present call
}


void DepthOfFieldController::setXFocusDelta(reshade::api::effect_runtime* runtime, float newValueX)
{
	if(DepthOfFieldControllerState::Setup!=_state)
	{
		// not in setup
		return;
	}
	_focusDelta = newValueX;

	calculateShapePoints();

	// set the uniform in the shader for blending the new framebuffer so the user has visual feedback
	setUniformFloatVariable(runtime, "FocusDelta", _focusDelta);
	// the value is passed to the shader next present call
}


void DepthOfFieldController::displayScreenshotSessionStartError(const ScreenshotSessionStartReturnCode sessionStartResult)
{
	std::string reason = "Unknown error.";
	switch(sessionStartResult)
	{
		case ScreenshotSessionStartReturnCode::Error_CameraNotEnabled:
			reason = "you haven't enabled the camera.";
			break;
		case ScreenshotSessionStartReturnCode::Error_CameraPathPlaying:
			reason = "there's a camera path playing.";
			break;
		case ScreenshotSessionStartReturnCode::Error_AlreadySessionActive:
			reason = "there's already a session active.";
			break;
		case ScreenshotSessionStartReturnCode::Error_CameraFeatureNotAvailable:
			reason = "the camera feature isn't available in the tools.";
			break;
	}
	OverlayControl::addNotification("Depth-of-field session couldn't be started: " + reason);
}


void DepthOfFieldController::writeVariableStateToShader(reshade::api::effect_runtime* runtime)
{
	if(isReshadeStateEmpty())
	{
		std::scoped_lock lock(_reshadeStateMutex);
		_reshadeStateAtStart.obtainReshadeState(runtime);
	}

	setUniformIntVariable(runtime, "SessionState", (int)_state);
	setUniformFloatVariable(runtime, "FocusDelta", _focusDelta);
	setUniformBoolVariable(runtime, "BlendFrame", _blendFrame);
	setUniformFloatVariable(runtime, "BlendFactor", _blendFactor);
	setUniformFloat2Variable(runtime, "AlignmentDelta", _xAlignmentDelta, _yAlignmentDelta);
	setUniformFloatVariable(runtime, "HighlightBoost", _highLightBoostForFrame);// _highlightBoostFactor);
	setUniformFloatVariable(runtime, "HighlightGammaFactor", _highlightGammaFactor);
	setUniformBoolVariable(runtime, "ShowMagnifier", _magnificationSettings.ShowMagnifier);
	setUniformFloatVariable(runtime, "MagnificationFactor", _magnificationSettings.MagnificationFactor);
	setUniformFloat2Variable(runtime, "MagnificationArea", _magnificationSettings.WidthMagnifierArea, _magnificationSettings.HeightMagnifierArea);
	setUniformFloat2Variable(runtime, "MagnificationLocationCenter", _magnificationSettings.XMagnifierLocation, _magnificationSettings.YMagnifierLocation);
}


void DepthOfFieldController::loadIniFileData(CDataFile& iniFile)
{
	loadFloatFromIni(iniFile, "MaxBokehSize", &_maxBokehSize);
	loadFloatFromIni(iniFile, "HighlightBoostFactor", &_highlightBoostFactor);
	loadFloatFromIni(iniFile, "HighlightGammaFactor", &_highlightGammaFactor);
	loadFloatFromIni(iniFile, "MagnificationAreaWidth", &_magnificationSettings.WidthMagnifierArea);
	loadFloatFromIni(iniFile, "MagnificationAreaHeight", &_magnificationSettings.HeightMagnifierArea);
	loadFloatFromIni(iniFile, "AnamorphicFactor", &_anamorphicFactor);
	loadFloatFromIni(iniFile, "RingAngleOffset", &_ringAngleOffset);
	loadFloatFromIni(iniFile, "RotationAngle", &_apertureShapeSettings.RotationAngle);
	loadFloatFromIni(iniFile, "RoundFactor", &_apertureShapeSettings.RoundFactor);
	loadFloatFromIni(iniFile, "SphericalAberrationFactor", &_sphericalAberrationFactor);
	loadFloatFromIni(iniFile, "SphericalAberrationDimFactor", &_sphericalAberrationDimFactor);
	loadIntFromIni(iniFile, "NumberOfVertices", &_apertureShapeSettings.NumberOfVertices);
	loadIntFromIni(iniFile, "Quality", &_quality);
	loadIntFromIni(iniFile, "NumberOfPointsInnermostRing", &_numberOfPointsInnermostRing);
	loadIntFromIni(iniFile, "NumberOfFramesToWaitPerFrame", &_numberOfFramesToWaitPerFrame);
	loadBoolFromIni(iniFile, "ShowProgressBarAsOverlay", &_showProgressBarAsOverlay, true);

	int blurType = 0;
	loadIntFromIni(iniFile, "BlurType", &blurType);
	_blurType = (DepthOfFieldBlurType)blurType;
}


void DepthOfFieldController::saveIniFileData(CDataFile& iniFile)
{
	iniFile.SetFloat("MaxBokehSize", _maxBokehSize, "", "DepthOfField");
	iniFile.SetFloat("HighlightBoostFactor", _highlightBoostFactor, "", "DepthOfField");
	iniFile.SetFloat("HighlightGammaFactor", _highlightGammaFactor, "", "DepthOfField");
	iniFile.SetFloat("MagnificationAreaWidth", _magnificationSettings.WidthMagnifierArea, "", "DepthOfField");
	iniFile.SetFloat("MagnificationAreaHeight", _magnificationSettings.HeightMagnifierArea, "", "DepthOfField");
	iniFile.SetFloat("AnamorphicFactor", _anamorphicFactor, "", "DepthOfField");
	iniFile.SetFloat("RingAngleOffset", _ringAngleOffset, "", "DepthOfField");
	iniFile.SetFloat("RotationAngle", _apertureShapeSettings.RotationAngle, "", "DepthOfField");
	iniFile.SetFloat("RoundFactor", _apertureShapeSettings.RoundFactor, "", "DepthOfField");
	iniFile.SetFloat("SphericalAberrationFactor", _sphericalAberrationFactor, "", "DepthOfField");
	iniFile.SetFloat("SphericalAberrationDimFactor", _sphericalAberrationDimFactor, "", "DepthOfField");
	iniFile.SetInt("NumberOfVertices", _apertureShapeSettings.NumberOfVertices, "", "DepthOfField");
	iniFile.SetInt("Quality", _quality, "", "DepthOfField");
	iniFile.SetInt("NumberOfPointsInnermostRing", _numberOfPointsInnermostRing, "", "DepthOfField");
	iniFile.SetInt("NumberOfFramesToWaitPerFrame", _numberOfFramesToWaitPerFrame, "", "DepthOfField");
	iniFile.SetBool("ShowProgressBarAsOverlay", _showProgressBarAsOverlay, "", "DepthOfField");
	iniFile.SetInt("BlurType", (int)_blurType, "", "DepthOfField");
}


void DepthOfFieldController::startSession(reshade::api::effect_runtime* runtime)
{
	if(!_cameraToolsConnector.cameraToolsConnected())
	{
		return;
	}
	const auto sessionStartResult = _cameraToolsConnector.startScreenshotSession((uint8_t)ScreenshotType::MultiShot);
	if(sessionStartResult!=ScreenshotSessionStartReturnCode::AllOk)
	{
		displayScreenshotSessionStartError(sessionStartResult);
		return;
	}

	calculateShapePoints();

	{
		std::scoped_lock lock(_reshadeStateMutex);
		_reshadeStateAtStart.obtainReshadeState(runtime);
	}

	// set uniform variable 'SessionState' to 1 (start)
	_state = DepthOfFieldControllerState::Start;
	_renderPaused = false;
	setUniformIntVariable(runtime, "SessionState", (int)_state);
	_highLightBoostForFrame = _highLightBoostForFrame * (1 - _sphericalAberrationFactor);	// center pixel
	// set framecounter to 3 so we wait 3 frames before moving on to 'Setup'
	_onPresentWorkCounter = 3;	// wait 3 frames
	_onPresentWorkFunc = [&](reshade::api::effect_runtime* r)
	{
		this->_state = DepthOfFieldControllerState::Setup;
		// we have to move the camera over the new distance. We move relative to the start position.
		_cameraToolsConnector.moveCameraMultishot(_maxBokehSize, 0.0f, 0.0f, true);
	};
}


void DepthOfFieldController::endSession(reshade::api::effect_runtime* runtime)
{
	_state = DepthOfFieldControllerState::Off;
	_renderPaused = false;
	setUniformIntVariable(runtime, "SessionState", (int)_state);

	if(_cameraToolsConnector.cameraToolsConnected())
	{
		_cameraToolsConnector.endScreenshotSession();
	}
}


void DepthOfFieldController::reshadeBeginEffectsCalled(reshade::api::effect_runtime* runtime)
{
	if(nullptr==runtime || !_cameraToolsConnector.cameraToolsConnected())
	{
		return;
	}
	// First handle data changing work
	if(_onPresentWorkCounter<=0)
	{
		_onPresentWorkCounter = 0;

		if(nullptr!=_onPresentWorkFunc)
		{
			const std::function<void(reshade::api::effect_runtime*)> funcToCall = _onPresentWorkFunc;
			// reset the current func to nullptr so next time we won't call it again.
			_onPresentWorkFunc = nullptr;

			funcToCall(runtime);
		}
	}
	else
	{
		_onPresentWorkCounter--;
	}

	if(DepthOfFieldControllerState::Rendering== _state)
	{
		handlePresentBeforeReshadeEffects();
	}

	// Then make sure the shader knows our changed data...

	// always write the variables, as otherwise they'll lose their value when the user e.g. hotsamples.
	writeVariableStateToShader(runtime);
}


void DepthOfFieldController::reshadeFinishEffectsCalled(reshade::api::effect_runtime* runtime)
{
	if(nullptr == runtime || !_cameraToolsConnector.cameraToolsConnected())
	{
		return;
	}

	if(DepthOfFieldControllerState::Rendering == _state)
	{
		handlePresentAfterReshadeEffects();
	}
}


void DepthOfFieldController::performRenderFrameSetupWork()
{
	// move camera and set counter and move to next state
	const auto& currentFrameData = _cameraSteps[_currentFrame];
	_cameraToolsConnector.moveCameraMultishot(currentFrameData.xDelta, currentFrameData.yDelta, 0.0f, true);
	_xAlignmentDelta = currentFrameData.xAlignmentDelta;
	_yAlignmentDelta = currentFrameData.yAlignmentDelta;
	_frameWaitCounter = _numberOfFramesToWaitPerFrame;
	_blendFactor = 1.0f / (static_cast<float>(_currentFrame) + 2.0f);		// frame start at 0 so +1, and we have to blend the original too, so +1
	_highLightBoostForFrame = _highlightBoostFactor * currentFrameData.busyBokehFactor;
	// Set the framestate to wait so the counter will take effect.
	_renderFrameState = DepthOfFieldRenderFrameState::FrameWait;
}


void DepthOfFieldController::handlePresentBeforeReshadeEffects()
{
	if(_state!=DepthOfFieldControllerState::Rendering)
	{
		return;
	}

	switch(_renderFrameState)
	{
		case DepthOfFieldRenderFrameState::Off:
		case DepthOfFieldRenderFrameState::FrameBlending:
			// no-op
			break;
		case DepthOfFieldRenderFrameState::Start:
			// start state of the whole process. Only arriving here once per render session.
			performRenderFrameSetupWork();
			break;
		case DepthOfFieldRenderFrameState::FrameWait:
			{
				// check if counter is 0. If so, switch to next state, if not, decrease and do nothing
				if(_frameWaitCounter <= 0)
				{
					_frameWaitCounter = 0;
					// Ready to blend. As we're currently before the reshade effects are handled but after the frame has been drawn by the engine
					// we can set blendFrame to true here and the shader will blend the current framebuffer this frame.
					// This works because after this method, the uniforms are written to the shader, so the shader will pick the new value up
					// when it's being drawn 
					_blendFrame = true;
					// Setting the state to blending as we're blending after this method. The handling of this event is done in handlePresentAfterReshadeEffects
					_renderFrameState = DepthOfFieldRenderFrameState::FrameBlending;
				}
				else
				{
					_frameWaitCounter--;
				}
			}
			break;
	}
}


void DepthOfFieldController::handlePresentAfterReshadeEffects()
{
	if(_state != DepthOfFieldControllerState::Rendering)
	{
		return;
	}

	switch(_renderFrameState)
	{
		case DepthOfFieldRenderFrameState::Off:
		case DepthOfFieldRenderFrameState::Start:
		case DepthOfFieldRenderFrameState::FrameWait:
			// no-op
			break;
		case DepthOfFieldRenderFrameState::FrameBlending:
			{
				// Blending work has taken place, we're now done with that as the shader has run. We switch it off by resetting the variable.
				// This variable is written to the shader at the end of the handler called before the reshade effects will be rendered, so
				// it will take effect then. (the shader isn't run before that point so it's ok).
				_blendFrame = false;
				if(!_renderPaused)
				{
					_currentFrame++;
					if(_currentFrame >= _numberOfFramesToRender)
					{
						// we're done rendering
						_renderFrameState = DepthOfFieldRenderFrameState::Off;
						_state = DepthOfFieldControllerState::Done;
						reshade::log_message(reshade::log_level::info, "Dof render session completed");
					}
					else
					{
						// back to setup for the next frame
						performRenderFrameSetupWork();
					}
				}
			}
			break;
	}
}


float DepthOfFieldController::calculateSphericalAberrationFactorToUse(const int startRingBoosted, int ringNo)
{
	float toReturn = 1.0f;
	if(ringNo >= startRingBoosted || startRingBoosted==1)
	{
		toReturn = 1.0f;
	}
	else
	{
		toReturn = (((float)ringNo / (float)(startRingBoosted - 1)) * (1.0f - _sphericalAberrationDimFactor)) + (1.0f - _sphericalAberrationDimFactor);
		toReturn = IGCS::Utils::clampEx(toReturn, 0.0f, 1.0f);
	}
	return toReturn;
}


void DepthOfFieldController::createCircleDoFPoints()
{
	_cameraSteps.clear();

	const float pointsFirstRing = (float)_numberOfPointsInnermostRing;
	float pointsOnRing = pointsFirstRing;
	const float maxBokehRadius = _maxBokehSize / 2.0f;
	const float focusDeltaHalf = _focusDelta / 2.0f;
	const int startRingBoosted = (int)(_sphericalAberrationFactor * (float)(_quality-1)) +1;
	for(int ringNo = 1; ringNo <= _quality; ringNo++)
	{
		const float anglePerPoint = 6.28318530717958f / pointsOnRing;
		float angle = anglePerPoint + ((float)ringNo * _ringAngleOffset);
		const float ringDistance = (float)ringNo / (float)_quality;
		const float sphericalAberrationFactorTouse = calculateSphericalAberrationFactorToUse(startRingBoosted, ringNo);
		for(int pointNumber = 0;pointNumber<pointsOnRing;pointNumber++)
		{
			const float sinAngle = sin(angle);
			const float cosAngle = cos(angle);
			const float x = ringDistance * cosAngle * _anamorphicFactor;
			const float y = ringDistance * sinAngle;
			const float xDelta = maxBokehRadius * x;
			const float yDelta = maxBokehRadius * y;
			_cameraSteps.push_back({ xDelta, yDelta, x * -focusDeltaHalf, y * focusDeltaHalf, sphericalAberrationFactorTouse});
			angle += anglePerPoint;
			angle = fmod(angle, 6.28318530717958f);
		}

		pointsOnRing += pointsFirstRing;
	}

	switch(_renderOrder)
	{
		case DepthOfFieldRenderOrder::InnerRingToOuterRing:
			// nothing, we're already having the points in the right order
			break;
		case DepthOfFieldRenderOrder::OuterRingToInnerRing:
			// reverse the container.
			std::ranges::reverse(_cameraSteps);
			break;
		case DepthOfFieldRenderOrder::Randomized:
			std::ranges::shuffle(_cameraSteps, std::random_device());
			break;
		default: ;
	}
}


void DepthOfFieldController::createApertureShapedDoFPoints()
{
	_cameraSteps.clear();

	// sanitize input for 4 vertex elements
	if(4 == _apertureShapeSettings.NumberOfVertices)
	{
		if(_ringAngleOffset<-0.015f || _ringAngleOffset > 0.015f)
		{
			_ringAngleOffset = 0.0f;
		}
	}

	const float maxBokehRadius = _maxBokehSize / 2.0f;
	const float focusDeltaHalf = _focusDelta / 2.0f;
	const float anglePerVertex = 6.28318530717958f / (float)_apertureShapeSettings.NumberOfVertices;
	const int startRingBoosted = (int)(_sphericalAberrationFactor * (float)(_quality - 1)) + 1;
	for(int ringNo = 1; ringNo <= _quality; ringNo++)
	{
		// ring angle offset is applied stronger on inner rings than on outer rings, to keep the outer ring from staying in the same place. 
		float vertexAngle = fmod(anglePerVertex + (_apertureShapeSettings.RotationAngle * 6.28318530717958f) + ((float)(_quality-ringNo) * _ringAngleOffset), 6.28318530717958f);
		const float ringDistance = (float)ringNo / (float)_quality;
		const float sphericalAberrationFactorTouse = calculateSphericalAberrationFactorToUse(startRingBoosted, ringNo);
		for(int vertexNo = 0; vertexNo < _apertureShapeSettings.NumberOfVertices; vertexNo++)
		{
			const float sinAngleCurrentVertex = sin(vertexAngle);
			const float cosAngleCurrentVertex = cos(vertexAngle);
			const float nextVertexAngle = fmod(vertexAngle + anglePerVertex, 6.28318530717958f);
			const float sinAngleNextVertex = sin(nextVertexAngle);
			const float cosAngleNextVertex = cos(nextVertexAngle);
			const float xCurrentVertex = ringDistance * cosAngleCurrentVertex * _anamorphicFactor;
			const float yCurrentVertex = ringDistance * sinAngleCurrentVertex;
			const float xNextVertex = ringDistance * cosAngleNextVertex * _anamorphicFactor;
			const float yNextVertex = ringDistance * sinAngleNextVertex;
			const float pointStepSize = 1.0f / (float)ringNo;
			float pointStep = pointStepSize;
			for(int pointNumber = 0; pointNumber < ringNo; pointNumber++)
			{
				const float pointAngle = IGCS::Utils::lerp(vertexAngle, vertexAngle + anglePerVertex, pointStep);
				const float sinPointAngle = sin(pointAngle);
				const float cosPointAngle = cos(pointAngle);
				const float xRoundPoint = ringDistance * cosPointAngle * _anamorphicFactor;
				const float yRoundPoint = ringDistance * sinPointAngle;
				const float xLinePoint = IGCS::Utils::lerp(xCurrentVertex, xNextVertex, pointStep);
				const float yLinePoint = IGCS::Utils::lerp(yCurrentVertex, yNextVertex, pointStep);
				const float x = IGCS::Utils::lerp(xLinePoint, xRoundPoint, _apertureShapeSettings.RoundFactor);
				const float y = IGCS::Utils::lerp(yLinePoint, yRoundPoint, _apertureShapeSettings.RoundFactor);
				const float xDelta = maxBokehRadius * x;
				const float yDelta = maxBokehRadius * y;
				_cameraSteps.push_back({ xDelta, yDelta, x * -focusDeltaHalf, y * focusDeltaHalf, sphericalAberrationFactorTouse });
				pointStep += pointStepSize;
			}
			vertexAngle += anglePerVertex;
			vertexAngle = fmod(vertexAngle, 6.28318530717958f);
		}
	}

	switch(_renderOrder)
	{
		case DepthOfFieldRenderOrder::InnerRingToOuterRing:
			// nothing, we're already having the points in the right order
			break;
		case DepthOfFieldRenderOrder::OuterRingToInnerRing:
			// reverse the container.
			std::ranges::reverse(_cameraSteps);
			break;
		case DepthOfFieldRenderOrder::Randomized:
			std::ranges::shuffle(_cameraSteps, std::random_device());
			break;
		default:;
	}
}


void DepthOfFieldController::calculateShapePoints()
{
	switch(_blurType)
	{
		case DepthOfFieldBlurType::ApertureShape:
			createApertureShapedDoFPoints();
			break;
		case DepthOfFieldBlurType::Circular: 
			createCircleDoFPoints();
			break;
	}
}


void DepthOfFieldController::startRender(reshade::api::effect_runtime* runtime)
{
	if(nullptr == runtime || !_cameraToolsConnector.cameraToolsConnected())
	{
		return;
	}

	if(_state!=DepthOfFieldControllerState::Setup)
	{
		// not in the right previous state
		return;
	}

	reshade::log_message(reshade::log_level::info, "Dof render session started");

	// set initial shader start state
	_blendFactor = 0.0f;
	_currentFrame = 0;
	_numberOfFramesToRender = _cameraSteps.size();
	_renderFrameState = DepthOfFieldRenderFrameState::Start;
	_state = DepthOfFieldControllerState::Rendering;
}


void DepthOfFieldController::migrateReshadeState(reshade::api::effect_runtime* runtime)
{
	if(!_cameraToolsConnector.cameraToolsConnected())
	{
		return;
	}
	switch(_state)
	{
		case DepthOfFieldControllerState::Cancelling:
			return;
	}
	if(isReshadeStateEmpty())
	{
		return;
	}

	{
		std::scoped_lock lock(_reshadeStateMutex);
		ReshadeStateSnapshot newState;
		newState.obtainReshadeState(runtime);
		// we don't care about the variable values, only about id's and variable names. So we can replace what we have with the new state.
		// If the new state is empty, that's fine, setting variables takes care of that. 
		_reshadeStateAtStart = newState;
	}
		
	// if the newstate is empty we do nothing. If the new state isn't empty we had a migration and the variables are valid.
	if(!isReshadeStateEmpty() && _state == DepthOfFieldControllerState::Setup)
	{
		// we now restart the session. This is necessary because we lose the cached start texture.
		endSession(runtime);
		startSession(runtime);
	}
}


void DepthOfFieldController::drawShape(ImDrawList* drawList, ImVec2 topLeftScreenCoord, float canvasWidthHeight)
{
	if(_cameraSteps.size()<=0)
	{
		return;
	}

	const float x = canvasWidthHeight / 2.0f + topLeftScreenCoord.x;
	const float y = canvasWidthHeight / 2.0f + topLeftScreenCoord.y;
	const float maxRadius = (canvasWidthHeight / 2.0f)-5.0f;	// to have some space around the edge
	float maxBokehRadius = _maxBokehSize / 2.0f;
	maxBokehRadius = maxBokehRadius < FLT_EPSILON ? 1.0f : maxBokehRadius;
	const float centerSpotSphericalAberrationFactorToUse = calculateSphericalAberrationFactorToUse((int)(_sphericalAberrationFactor * (float)(_quality - 1)) + 1, 1);
	ImColor dotColor = ImColor(centerSpotSphericalAberrationFactorToUse, centerSpotSphericalAberrationFactorToUse, centerSpotSphericalAberrationFactorToUse);
	drawList->AddCircleFilled(ImVec2(x, y), 1.5f, dotColor);	// center
	for(const auto& step : _cameraSteps)
	{
		dotColor = ImColor(step.busyBokehFactor, step.busyBokehFactor, step.busyBokehFactor);
		// our (0,0) for rendering is top left, however the (0, 0) for the canvas is bottom left.
		drawList->AddCircleFilled(ImVec2(x + ((step.xDelta / maxBokehRadius) * maxRadius), y - ((step.yDelta / maxBokehRadius) * maxRadius)), 1.5f, dotColor);
	}
}


void DepthOfFieldController::renderProgressBar()
{
	const int totalAmountOfSteps = _cameraSteps.size();
	const float progress = (float)_currentFrame / (float)totalAmountOfSteps;
	const float progress_saturated = IGCS::Utils::clampEx(progress, 0.0f, 1.0f);
	char buf[128];
	sprintf(buf, "%d/%d", (int)(progress_saturated * totalAmountOfSteps), totalAmountOfSteps);
	ImGui::ProgressBar(progress, ImVec2(0.f, 0.f), buf);
}


void DepthOfFieldController::renderOverlay()
{
	if(_state!=DepthOfFieldControllerState::Rendering || _cameraSteps.size()<=0 || !_showProgressBarAsOverlay)
	{
		return;
	}

	ImGui::SetNextWindowBgAlpha(0.9f);
	ImGui::SetNextWindowPos(ImVec2(10, 10));
	if(ImGui::Begin("IgcsConnector_DoFProgress", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
	{
		renderProgressBar();
	}
	ImGui::End();
}


void DepthOfFieldController::setUniformIntVariable(reshade::api::effect_runtime* runtime, const std::string& uniformName, int valueToWrite)
{
	std::scoped_lock lock(_reshadeStateMutex);
	_reshadeStateAtStart.setUniformIntVariable(runtime, "IgcsDof.fx", uniformName, valueToWrite);
}


void DepthOfFieldController::setUniformFloatVariable(reshade::api::effect_runtime* runtime, const std::string& uniformName, float valueToWrite)
{
	std::scoped_lock lock(_reshadeStateMutex);
	_reshadeStateAtStart.setUniformFloatVariable(runtime, "IgcsDof.fx", uniformName, valueToWrite);
}


void DepthOfFieldController::setUniformBoolVariable(reshade::api::effect_runtime* runtime, const std::string& uniformName, bool valueToWrite)
{
	std::scoped_lock lock(_reshadeStateMutex);
	_reshadeStateAtStart.setUniformBoolVariable(runtime, "IgcsDof.fx", uniformName, valueToWrite);
}


void DepthOfFieldController::setUniformFloat2Variable(reshade::api::effect_runtime* runtime, const std::string& uniformName, float value1ToWrite, float value2ToWrite)
{
	std::scoped_lock lock(_reshadeStateMutex);
	_reshadeStateAtStart.setUniformFloat2Variable(runtime, "IgcsDof.fx", uniformName, value1ToWrite, value2ToWrite);
}


void DepthOfFieldController::loadFloatFromIni(CDataFile& iniFile, const std::string& key, float* toWriteTo)
{
	if(nullptr == toWriteTo)
	{
		return;
	}
	const float value = iniFile.GetFloat(key, "DepthOfField");
	if(value != FLT_MIN)
	{
		*toWriteTo = value;
	}
}


void DepthOfFieldController::loadIntFromIni(CDataFile& iniFile, const std::string& key, int* toWriteTo)
{
	if(nullptr == toWriteTo)
	{
		return;
	}
	const float value = iniFile.GetInt(key, "DepthOfField");
	if(value != INT_MIN)
	{
		*toWriteTo = value;
	}
}

void DepthOfFieldController::loadBoolFromIni(CDataFile& iniFile, const std::string& key, bool* toWriteTo, bool defaultValue)
{
	if(nullptr==toWriteTo)
	{
		return;
	}
	// a little inefficient, but getkey is protected
	const auto boolAsString = iniFile.GetValue(key, "DepthOfField");
	bool valueToUse = defaultValue;
	if(boolAsString.length()>0)
	{
		valueToUse = iniFile.GetBool(key, "DepthOfField");
	}
	*toWriteTo = valueToUse;
}

