#pragma once
#include "Camera.h"
#include "HavokSkeleton.h"

class ViewportPanel {
public:
    void Draw();
private:
    Camera   m_camera;
    Skeleton m_skeleton;
    char     m_loadErr[256] = {};

    void OpenSkeletonDialog();
    void FrameSkeleton();
};
