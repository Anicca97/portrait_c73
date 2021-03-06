/**
* Copyright 2020 Huawei Technologies Co., Ltd
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

* http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

* File sample_process.cpp
* Description: handle acl resource
*/
#include <cstring>
#include <iostream>

#include "acl/acl.h"
#include "model_process.h"
#include "utils.h"
#include "object_detect.h"
using namespace std;

namespace {
 const static std::vector<std::string> ssdLabel = { "background", "face"};

uint32_t gSendNum = 0;


enum BBoxIndex { EMPTY = 0, LABEL,SCORE,TOPLEFTX,TOPLEFTY, BOTTOMRIGHTX, BOTTOMRIGHTY};
}

ObjectDetect::ObjectDetect(const char* modelPath,
                           uint32_t modelWidth,
                           uint32_t modelHeight)
:deviceId_(0), context_(nullptr), stream_(nullptr), modelWidth_(modelWidth),
 modelHeight_(modelHeight), isInited_(false){
    imageInfoSize_ = 0;
    imageInfoBuf_ = nullptr;
    modelPath_ = modelPath;
    Channel* chan = nullptr;
    PresenterErrorCode openChannelret = OpenChannelByConfig(chan, "../data/param.conf");
    if (openChannelret != PresenterErrorCode::kNone) {
        ERROR_LOG("Open channel failed, error %d\n", (int)openChannelret);
    }
    chan_.reset(chan);

}

ObjectDetect::~ObjectDetect() {
    DestroyResource();
}

Result ObjectDetect::InitResource() {
    // ACL init
    const char *aclConfigPath = "../src/acl.json";
    aclError ret = aclInit(aclConfigPath);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl init failed\n");
        return FAILED;
    }
    INFO_LOG("acl init success\n");

    // open device
    ret = aclrtSetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl open device %d failed\n", deviceId_);
        return FAILED;
    }
    INFO_LOG("open device %d success\n", deviceId_);

    // create context (set current)
    ret = aclrtCreateContext(&context_, deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl create context failed\n");
        return FAILED;
    }
    INFO_LOG("create context success");

    // create stream
    ret = aclrtCreateStream(&stream_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl create stream failed\n");
        return FAILED;
    }
    INFO_LOG("create stream success");

    ret = aclrtGetRunMode(&runMode_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl get run mode failed\n");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::InitModel(const char* omModelPath) {
    Result ret = model_.LoadModelFromFileWithMem(omModelPath);
    if (ret != SUCCESS) {
        ERROR_LOG("execute LoadModelFromFileWithMem failed\n");
        return FAILED;
    }

    ret = model_.CreateDesc();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateDesc failed\n");
        return FAILED;
    }

    ret = model_.CreateOutput();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateOutput failed\n");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::CreateImageInfoBuffer()
{
    const float imageInfo[4] = {(float)modelWidth_, (float)modelHeight_,
    (float)modelWidth_, (float)modelHeight_};
    imageInfoSize_ = sizeof(imageInfo);
    if (runMode_ == ACL_HOST)
        imageInfoBuf_ = Utils::CopyDataHostToDevice((void *)imageInfo, imageInfoSize_);
    else
        imageInfoBuf_ = Utils::CopyDataDeviceToDevice((void *)imageInfo, imageInfoSize_);
    if (imageInfoBuf_ == nullptr) {
        ERROR_LOG("Copy image info to device failed\n");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::Init() {
    if (isInited_) {
        INFO_LOG("Classify instance is initied already!\n");
        return SUCCESS;
    }

    Result ret = InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("Init acl resource failed\n");
        return FAILED;
    }

    ret = InitModel(modelPath_);
    if (ret != SUCCESS) {
        ERROR_LOG("Init model failed\n");
        return FAILED;
    }

    ret = dvpp_.InitResource(stream_);
    if (ret != SUCCESS) {
        ERROR_LOG("Init dvpp failed\n");
        return FAILED;
    }

    ret = CreateImageInfoBuffer();
    if (ret != SUCCESS) {
        ERROR_LOG("Create image info buf failed\n");
        return FAILED;
    }

    isInited_ = true;
    return SUCCESS;
}

Result ObjectDetect::Preprocess(ImageData& resizedImage, ImageData & srcImage) {
    //resize
    Result ret = dvpp_.Resize(resizedImage, srcImage, modelWidth_, modelHeight_);
    if (ret == FAILED) {
        ERROR_LOG("Resize image failed\n");
        return FAILED;
    }
    INFO_LOG("Resize image success\n");
    return SUCCESS;
}

Result ObjectDetect::Inference(aclmdlDataset*& inferenceOutput,
                               ImageData& resizedImage) {
   Result ret = model_.CreateInput(resizedImage.data.get(),
                                    resizedImage.size);
    if (ret != SUCCESS) {
        ERROR_LOG("Create mode input dataset failed\n");
        return FAILED;
    }

    ret = model_.Execute();
    if (ret != SUCCESS) {
        model_.DestroyInput();
        ERROR_LOG("Execute model inference failed\n");
        return FAILED;
    }
    model_.DestroyInput();

    inferenceOutput = model_.GetModelOutputData();
    return SUCCESS;
}

Result ObjectDetect::Postprocess(ImageData& image, aclmdlDataset* modelOutput) {
    uint32_t dataSize = 0;
    float* detectData = (float *)GetInferenceOutputItem(dataSize, modelOutput, 0);
    if (detectData == nullptr)
        return FAILED;

    vector<DetectionResult> detectResults;

    uint8_t* mask = new uint8_t[100352];
    for (uint32_t i = 0; i < 100352; i++) {
        if (detectData[i] >= 1) {
            mask[i] = 255;
        }
        else if (detectData[i] <= 0) {
            mask[i] = 0;
        }
        else {
            mask[i] = (uint8_t) (detectData[i] * 255);
        }
    }

    Result ret = SendImage(chan_.get(), image, mask, detectResults);
    return ret;
}

void* ObjectDetect::GetInferenceOutputItem(uint32_t& itemDataSize,
                                           aclmdlDataset* inferenceOutput,
                                           uint32_t idx) {
    aclDataBuffer* dataBuffer = aclmdlGetDatasetBuffer(inferenceOutput, idx);
    if (dataBuffer == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer from model "
                  "inference output failed\n", idx);
        return nullptr;
    }

    void* dataBufferDev = aclGetDataBufferAddr(dataBuffer);
    if (dataBufferDev == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer address "
                  "from model inference output failed\n", idx);
        return nullptr;
    }

    size_t bufferSize = aclGetDataBufferSize(dataBuffer);
    if (bufferSize == 0) {
        ERROR_LOG("The %d   th dataset buffer size of "
                  "model inference output is 0\n", idx);
        return nullptr;
    }

    void* data = nullptr;
    data = dataBufferDev;

    itemDataSize = bufferSize;
    return data;
}

void ObjectDetect::DestroyResource()
{
    aclError ret = aclrtResetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("reset device failed\n");
    }
    INFO_LOG("end to reset device is %d\n", deviceId_);

    ret = aclFinalize();
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("finalize acl failed\n");
    }
    INFO_LOG("end to finalize acl");
    aclrtFree(imageInfoBuf_);
}
Result ObjectDetect::SendImage(Channel* channel, ImageData& jpegImage, uint8_t* mask, vector<DetectionResult>& detRes) {
    ImageFrame frame;
    frame.format = ImageFormat::kJpeg;
    frame.width = jpegImage.width;
    frame.height = jpegImage.height;
    frame.size = jpegImage.size+100352;

    unsigned char *data = new unsigned char[1482752];
    memcpy(data, jpegImage.data.get(), sizeof(char)*1382400);
    memcpy(data+1382400, mask, sizeof(char)*100352);
    frame.data = data;
    frame.detection_results = detRes;

    PresenterErrorCode ret = PresentImage(channel, frame);
    // send to presenter failed
    if (ret != PresenterErrorCode::kNone) {
        ERROR_LOG("Send JPEG image to presenter failed, error %d\n", (int)ret);
        return FAILED;
    }

    INFO_LOG("Send JPEG image to presenter success, ret %d,num =%d \n", (int)ret,gSendNum++);
    return SUCCESS;
}