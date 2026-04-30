#include "export/annotation/AnnotationEncoder.h"
#include <algorithm>
#include <cstdio>

std::vector<HkxAnnotation> EncodeFaceAnnotations(
    const std::vector<FaceAnnotation>& morphs)
{
    // TODO: implement — format: "MorphFace.<system>|<morphName>|<intWeight>"
    (void)morphs;
    return {};
}

std::vector<HkxAnnotation> EncodeAudioCues(
    const std::vector<AudioCueAnnotation>& cues)
{
    // TODO: implement — format: "AudioCue|<eventName>"
    (void)cues;
    return {};
}

std::vector<HkxAnnotation> EncodeCameraEvents(
    const std::vector<CameraEventAnnotation>& events)
{
    // TODO: implement — format: "CameraShot|<shotName>"
    (void)events;
    return {};
}

std::vector<HkxAnnotation> MergeAnnotations(
    std::vector<HkxAnnotation> a,
    std::vector<HkxAnnotation> b)
{
    a.insert(a.end(), std::make_move_iterator(b.begin()),
                      std::make_move_iterator(b.end()));
    std::stable_sort(a.begin(), a.end(),
        [](const HkxAnnotation& x, const HkxAnnotation& y) {
            return x.time < y.time;
        });
    return a;
}
