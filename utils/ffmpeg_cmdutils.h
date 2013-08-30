/*
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

#ifndef FFMPEG_CMDUTILS_H_
#define FFMPEG_CMDUTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

struct AVDictionary;
struct AVFormatContext;

extern AVDictionary *format_opts, *codec_opts;

AVDictionary **setup_find_stream_info_opts(AVFormatContext *, AVDictionary *);

#ifdef __cplusplus
}
#endif
#endif  // FFMPEG_CMDUTILS_H_
