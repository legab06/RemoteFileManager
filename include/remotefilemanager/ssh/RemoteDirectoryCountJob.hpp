#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>

namespace rfm::ssh
{

enum class RemoteDirectoryCountReadState { Entry, End, Error };

struct RemoteDirectoryCountRead {
    RemoteDirectoryCountReadState state{RemoteDirectoryCountReadState::Error};
    QString name;
    int error{0};
};

enum class RemoteDirectoryCountState { Pending, Completed, Failed, Cancelled };

class RemoteDirectoryCountJob final
{
  public:
    using ReadNext = std::function<RemoteDirectoryCountRead()>;
    using Close = std::function<void()>;
    using EntryHidden = std::function<bool(const QString&)>;

    static constexpr qsizetype entriesPerStep = 128;

    RemoteDirectoryCountJob(quint64 id, QString path, ReadNext readNext, Close close,
                            EntryHidden entryHidden);
    ~RemoteDirectoryCountJob();

    [[nodiscard]] RemoteDirectoryCountState step();
    void cancel();
    [[nodiscard]] quint64 id() const;
    [[nodiscard]] const QString& path() const;
    [[nodiscard]] quint64 count() const;
    [[nodiscard]] int error() const;

  private:
    void close();

    quint64 m_id{0};
    QString m_path;
    ReadNext m_readNext;
    Close m_close;
    EntryHidden m_entryHidden;
    quint64 m_count{0};
    int m_error{0};
    RemoteDirectoryCountState m_state{RemoteDirectoryCountState::Pending};
    bool m_closed{false};
};

} // namespace rfm::ssh
