#pragma once

#include "waveform/widgets/waveformwidgetcategory.h"

struct WaveformWidgetVars {
    bool m_useGL;
    bool m_useGLES;
    bool m_useGLSL;
    WaveformWidgetCategory m_category;
};

// Which half of the waveform's vertical extent a widget displays. The
// waveform is mirrored about its zero line; Top shows the positive lobe
// with the zero line at the bottom edge of the widget, Bottom shows the
// negative lobe hanging from the top edge. Implemented by the allshader
// widget: geometry is rendered at virtual double height and the engine
// projection crops (and for Bottom, shifts) to the half.
enum class WaveHalf {
    None,
    Top,
    Bottom,
};
