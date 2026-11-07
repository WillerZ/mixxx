#pragma once

#include <QMatrix4x4>
#include <memory>
#include <vector>

namespace rendergraph {
class Engine;
class BaseNode;
} // namespace rendergraph

class rendergraph::Engine {
  public:
    Engine(std::unique_ptr<BaseNode> pRootNode);
    ~Engine();

    void render();
    // projYOffset shifts the projection rectangle down by that many world
    // units, cropping the projection to the band [projYOffset,
    // projYOffset + h] of a taller virtual scene (used by half-waveform
    // widgets). Default 0 keeps the full-scene projection.
    void resize(int w, int h, qreal projYOffset = 0.0);
    void preprocess();
    void add(BaseNode* pNode);
    void remove(BaseNode* pNode);
    const QMatrix4x4& matrix() const {
        return m_matrix;
    }

  private:
    void render(BaseNode* pNode);
    void resize(BaseNode* pNode, int, int);

    QMatrix4x4 m_matrix;
    std::unique_ptr<BaseNode> m_pRootNode;
    std::vector<BaseNode*> m_pPreprocessNodes;
    std::vector<BaseNode*> m_pInitializeNodes;
};
