#include "remotefilemanager/ssh/RemoteDeleteSafetyProbe.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QtEndian>

#include <QSet>

namespace rfm::ssh
{
namespace
{

constexpr auto WindowsMarker = "RFM_WINDOWS_DELETE_V1";
constexpr auto WindowsPathPrefix = "RFM_WINDOWS_DELETE_PATH_V1:";

bool completed(const rfm::core::VolumeCommandResult& result)
{
    return result.started && !result.timedOut && !result.crashed && !result.cancelled &&
           result.exitCode == 0;
}

QByteArray utf16LittleEndian(const QString& text)
{
    QByteArray encoded;
    encoded.reserve(text.size() * static_cast<qsizetype>(sizeof(ushort)));
    for (const QChar character : text) {
        const ushort value = qToLittleEndian(character.unicode());
        encoded.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    return encoded;
}

QString normalizedWindowsSftpPath(const QByteArray& encodedPath)
{
    const QString path = rfm::core::RemotePath::normalize(QString::fromUtf8(encodedPath));
    return path.startsWith(QChar{'/'}) ? path.toCaseFolded() : QString{};
}

} // namespace

rfm::core::RemoteMountPointState
RemoteDeleteSafetyProbe::portableSftpMountPointState(const RemoteDeleteSftpEvidence& evidence)
{
    if (!evidence.canonicalParent.has_value() || !evidence.canonicalChild.has_value() ||
        evidence.expectedChild.isEmpty()) {
        return rfm::core::RemoteMountPointState::Unknown;
    }
    if (*evidence.canonicalChild != evidence.expectedChild) {
        return rfm::core::RemoteMountPointState::MountPoint;
    }
    if (!evidence.parentFileSystem.has_value() || !evidence.childFileSystem.has_value()) {
        return rfm::core::RemoteMountPointState::Unknown;
    }
    return *evidence.parentFileSystem == *evidence.childFileSystem
               ? rfm::core::RemoteMountPointState::NotMountPoint
               : rfm::core::RemoteMountPointState::MountPoint;
}

QString RemoteDeleteSafetyProbe::windowsCommand(const QStringList& sftpDirectories)
{
    if (sftpDirectories.isEmpty()) {
        return {};
    }

    QStringList encodedDirectories;
    encodedDirectories.reserve(sftpDirectories.size());
    for (const QString& directory : sftpDirectories) {
        const QString normalized = rfm::core::RemotePath::normalize(directory);
        if (!normalized.startsWith(QChar{'/'}) || normalized.contains(QChar{'\0'})) {
            return {};
        }
        encodedDirectories.push_back(QString::fromLatin1(normalized.toUtf8().toBase64()));
    }

    const QString script = QStringLiteral(R"($ErrorActionPreference='Stop'
$rfmInputs=@(%1)
$rfmQueue=New-Object System.Collections.Queue
$rfmSeen=New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
function Write-RfmState([string]$state,[string]$fullName) {
  $rfmPath='/' + $fullName.Replace('\','/')
  $rfmEncoded=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($rfmPath))
  Write-Output ('RFM_WINDOWS_DELETE_PATH_V1:' + $state + ':' + $rfmEncoded)
}
foreach($rfmEncoded in $rfmInputs) {
  try {
    $rfmSftp=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($rfmEncoded))
    if($rfmSftp -notmatch '^/([A-Za-z]):/(.*)$') { Write-Output 'RFM_WINDOWS_DELETE_PATH_V1:UNKNOWN:'; continue }
    $rfmWindows=$matches[1] + ':\\' + $matches[2].Replace('/','\\')
    $rfmQueue.Enqueue($rfmWindows)
  } catch { Write-Output 'RFM_WINDOWS_DELETE_PATH_V1:UNKNOWN:' }
}
Write-Output 'RFM_WINDOWS_DELETE_V1'
while($rfmQueue.Count -gt 0) {
  $rfmCurrent=[string]$rfmQueue.Dequeue()
  if(-not $rfmSeen.Add($rfmCurrent)) { continue }
  try {
    $rfmItem=Get-Item -LiteralPath $rfmCurrent -Force -ErrorAction Stop
    if(-not $rfmItem.PSIsContainer) { Write-RfmState 'UNKNOWN' $rfmItem.FullName; continue }
    if(($rfmItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { Write-RfmState 'REPARSE' $rfmItem.FullName; continue }
    Write-RfmState 'SAFE' $rfmItem.FullName
    foreach($rfmChild in @(Get-ChildItem -LiteralPath $rfmItem.FullName -Force -ErrorAction Stop)) {
      if($rfmChild.PSIsContainer) { $rfmQueue.Enqueue($rfmChild.FullName) }
    }
  } catch { Write-Output 'RFM_WINDOWS_DELETE_PATH_V1:UNKNOWN:' }
}
)")
                               .arg(encodedDirectories.join(QStringLiteral("','"))
                                        .prepend(QChar{'\''})
                                        .append(QChar{'\''}));
    return QStringLiteral("powershell.exe -NoProfile -NonInteractive -EncodedCommand %1")
        .arg(QString::fromLatin1(utf16LittleEndian(script).toBase64()));
}

QHash<QString, rfm::core::RemoteMountPointState>
RemoteDeleteSafetyProbe::windowsMountPointStates(const rfm::core::VolumeCommandResult& result)
{
    QHash<QString, rfm::core::RemoteMountPointState> states;
    if (!completed(result)) {
        return states;
    }

    bool markerSeen = false;
    bool invalidOutput = false;
    for (const QString& line : result.standardOutput.split(QChar{'\n'})) {
        const QByteArray encodedLine = line.trimmed().toUtf8();
        if (encodedLine == WindowsMarker) {
            markerSeen = true;
            continue;
        }
        if (!encodedLine.startsWith(WindowsPathPrefix)) {
            continue;
        }
        const QList<QByteArray> fields = encodedLine.split(':');
        if (fields.size() != 3 || fields.at(2).isEmpty()) {
            invalidOutput = true;
            continue;
        }
        const QString path = normalizedWindowsSftpPath(QByteArray::fromBase64(fields.at(2)));
        rfm::core::RemoteMountPointState state = rfm::core::RemoteMountPointState::Unknown;
        if (fields.at(1) == QByteArrayLiteral("SAFE")) {
            state = rfm::core::RemoteMountPointState::NotMountPoint;
        } else if (fields.at(1) == QByteArrayLiteral("REPARSE")) {
            state = rfm::core::RemoteMountPointState::MountPoint;
        } else if (fields.at(1) != QByteArrayLiteral("UNKNOWN")) {
            invalidOutput = true;
        }
        if (path.isEmpty() || states.contains(path)) {
            invalidOutput = true;
            continue;
        }
        states.insert(path, state);
    }
    return markerSeen && !invalidOutput ? states
                                        : QHash<QString, rfm::core::RemoteMountPointState>{};
}

} // namespace rfm::ssh
