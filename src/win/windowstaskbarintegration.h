#ifndef WINDOWSTASKBARINTEGRATION_H
#define WINDOWSTASKBARINTEGRATION_H

#include <QObject>

class QEvent;

class QTimer;

namespace AnyKeep {

class WindowsTaskbarIntegration final : public QObject {
public:
    explicit WindowsTaskbarIntegration(QObject *parent = nullptr);
    ~WindowsTaskbarIntegration() override;

    static bool hasPackageIdentity();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void scheduleRebuild();
    void rebuildJumpList();
    void configureNoteWindow(QObject *object);
    void clearNoteWindowProperties(QObject *object);

    QTimer *rebuildTimer_ { nullptr };
    bool    comInitialized_ { false };
};

} // namespace AnyKeep

#endif // WINDOWSTASKBARINTEGRATION_H
