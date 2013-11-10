/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CODEC_UTILS_H_

#define CODEC_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Errors.h>
#include <media/stagefright/foundation/ABuffer.h>

namespace android {

sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd);
//Returns the sample rate based on the sampling frequency index
uint32_t getAACSampleRate(const uint8_t sf_index);
//Convert H.264 NAL format to annex b
status_t convertNal2AnnexB(uint8_t *dst, size_t dst_size,
        uint8_t *src, size_t src_size, size_t nal_len_size);

}  // namespace android

#endif  // CODEC_UTILS_H_
