//
// Created by uos on 25-1-16.
//

#ifndef IDLE_H
#define IDLE_H
#include <QObject>
#include <QTimer>

#define DUM_AUTO_IDLE_TIMEOUT 60000

class Idle : public QObject
{
    Q_OBJECT
public:
    explicit Idle(QObject *parent = nullptr);
    void Inhibit(const QString& task);
    void UnInhibit(const QString& task);

public Q_SLOTS:
    void onTimeout() const;
    void handleInhibit() const;
private:
    QTimer *m_timer{};
    QStringList m_reasons;

Q_SIGNALS:
    void inhibit();
};

#endif //IDLE_H
