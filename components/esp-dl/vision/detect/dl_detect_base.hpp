#pragma once

#include "dl_detect_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"

namespace dl {
namespace detect {
class Detect {
public:
    virtual ~Detect() {};
    virtual std::list<dl::detect::result_t> &run(const dl::image::img_t &img) = 0;
};

class DetectWrapper : public Detect {
protected:
    Detect *m_model;

public:
    ~DetectWrapper() { delete m_model; }
    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) { return m_model->run(img); }
};

class DetectImpl : public Detect {
protected:
    dl::Model *m_model = nullptr;
    dl::image::ImagePreprocessor *m_image_preprocessor = nullptr;
    dl::detect::DetectPostprocessor *m_postprocessor = nullptr;

public:
    ~DetectImpl();
    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;
};
} // namespace detect
} // namespace dl
