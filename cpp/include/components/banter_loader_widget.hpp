#pragma once

#include <array>

#include <QWidget>

class QParallelAnimationGroup;

namespace f2c_cpp {

class BanterLoaderWidget : public QWidget {
    Q_OBJECT

public:
    explicit BanterLoaderWidget(QWidget* parent = nullptr);

    void setDarkMode(bool dark_mode);
    bool isDarkMode() const { return dark_mode_; }

    void start();
    void stop();
    bool isRunning() const { return running_; }

private:
    void buildUi();
    void buildAnimations();
    void applyBoxStyles();
    QPoint baseVisiblePositionForIndex(int index) const;

    std::array<QWidget*, 9> boxes_ = {nullptr, nullptr, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, nullptr};
    QParallelAnimationGroup* animation_group_ = nullptr;
    bool dark_mode_ = false;
    bool running_ = false;
};

}  // namespace f2c_cpp
