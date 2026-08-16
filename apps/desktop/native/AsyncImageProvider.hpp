#pragma once

#include <QQuickAsyncImageProvider>

class AsyncImageProvider final : public QQuickAsyncImageProvider
{
public:
    explicit AsyncImageProvider(QString cacheRoot);
    ~AsyncImageProvider() override;

    QQuickImageResponse *requestImageResponse(
        const QString &id,
        const QSize &requestedSize) override;

private:
    const QString m_cacheRoot;
};
