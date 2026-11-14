// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/flags.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_file_origin.h"
#include "mtproto/sender.h"

#include <deque>

class PhotoData;
class DocumentData;

namespace Data {
class PhotoMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace AyuMediaDownload {

enum class MediaType : uint8 {
	Photo = 0x01,
	Video = 0x02,
	File = 0x04,
	Music = 0x08,
	Voice = 0x10,
	Round = 0x20,
};
inline constexpr bool is_flag_type(MediaType) { return true; }
using MediaTypes = base::flags<MediaType>;

[[nodiscard]] const std::vector<MediaType> &AllMediaTypes();

struct JobSettings {
	PeerId peerId = 0;
	PeerId migratedPeerId = 0;
	MsgId topicRootId = 0;
	QString folder;
	MediaTypes types;
	TimeId minDate = 0;
	TimeId maxDate = 0;
};

enum class JobStatus : uchar {
	Running,
	Paused,
	Finished,
	Failed,
};

enum class JobError : uchar {
	None,
	NoSpace,
	FolderUnavailable,
	Access,
};

struct JobProgress {
	JobStatus status = JobStatus::Paused;
	JobError error = JobError::None;
	QString errorDetails;
	int found = 0;
	int done = 0;
	int pending = 0;
	int unavailable = 0;
	bool discovering = false;
	TimeId waitUntil = 0;
	bool premiumLimited = false;
};

class Slots final {
public:
	explicit Slots(not_null<Main::Session*> session);

	void requestLimits();
	[[nodiscard]] bool tryAcquire(int32 dcId, bool large);
	void release(int32 dcId, bool large);
	[[nodiscard]] rpl::producer<> available() const;

private:
	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	int _smallLimit = 0;
	int _largeLimit = 0;
	bool _requested = false;
	base::flat_map<std::pair<int32, bool>, int> _active;
	rpl::event_stream<> _available;

};

class Job final : public base::has_weak_ptr {
public:
	Job(
		not_null<Main::Session*> session,
		not_null<Slots*> slots,
		JobSettings settings,
		Fn<void()> changed);
	~Job();

	[[nodiscard]] const JobSettings &settings() const;
	[[nodiscard]] JobProgress progress() const;
	[[nodiscard]] rpl::producer<> updates() const;

	void start();
	void pause();
	void discardReserved();

	[[nodiscard]] QByteArray serialize() const;
	[[nodiscard]] static std::unique_ptr<Job> Deserialize(
		not_null<Main::Session*> session,
		not_null<Slots*> slots,
		const QByteArray &data,
		Fn<void()> changed);

private:
	struct Entry {
		QString key;
		PeerId peerId = 0;
		MsgId msgId = 0;
		MediaType type = MediaType::File;
		TimeId date = 0;
		PhotoData *photo = nullptr;
		DocumentData *document = nullptr;
		int attempts = 0;
	};
	struct Active {
		Entry entry;
		QString path;
		int32 dcId = 0;
		bool large = false;
		bool started = false;
		std::shared_ptr<Data::PhotoMedia> photoView;
	};
	struct Cursor {
		PeerId peerId = 0;
		MediaType type = MediaType::File;
		MsgId offsetId = 0;
		bool exhausted = false;
	};
	enum class Outcome : uchar {
		Pending,
		Done,
		Retry,
		LocalError,
	};

	void notify();
	void requestNextPage();
	void processPage(int cursorIndex, const MTPmessages_Messages &result);
	void processMessage(
		PeerId peerId,
		MediaType type,
		const MTPMessage &message);
	void discoveryFailed(const MTP::Error &error);
	void downloadFloodWait(int seconds);
	void sweep();
	void fillSlots();
	[[nodiscard]] Outcome checkDocument(Active &active);
	[[nodiscard]] Outcome checkPhoto(Active &active);
	[[nodiscard]] bool startEntry(Entry entry);
	[[nodiscard]] JobError localProblem(int64 size) const;
	[[nodiscard]] bool downloadsBlocked() const;
	[[nodiscard]] bool isLarge(const Entry &entry) const;
	[[nodiscard]] int32 dcId(const Entry &entry) const;
	void finishActive(const QString &key, Outcome outcome);
	void markUnavailable(const QString &key);
	void checkFinished();
	void fail(JobError error, const QString &details = QString());
	void stopActive();
	[[nodiscard]] QString choosePath(const Entry &entry);
	[[nodiscard]] Data::FileOrigin origin(const Entry &entry) const;
	[[nodiscard]] Cursor *currentCursor();

	const not_null<Main::Session*> _session;
	const not_null<Slots*> _slots;
	const JobSettings _settings;
	const Fn<void()> _changed;
	MTP::Sender _api;

	JobStatus _status = JobStatus::Paused;
	JobError _error = JobError::None;
	QString _errorDetails;
	std::vector<Cursor> _cursors;
	std::deque<Entry> _queue;
	std::vector<Active> _active;
	base::flat_set<QString> _seen;
	base::flat_set<QString> _completed;
	base::flat_set<QString> _unavailable;
	base::flat_map<QString, QString> _reservedPaths;
	int _found = 0;
	int _unavailableCount = 0;
	mtpRequestId _searchRequestId = 0;
	TimeId _waitUntil = 0;
	TimeId _downloadWaitUntil = 0;
	crl::time _premiumLimitedTill = 0;
	base::Timer _discoveryTimer;
	base::Timer _sweepTimer;
	rpl::event_stream<> _updates;
	rpl::lifetime _lifetime;

};

class Manager final {
public:
	explicit Manager(not_null<Main::Session*> session);
	~Manager();

	[[nodiscard]] static not_null<Manager*> Instance(
		not_null<Main::Session*> session);

	[[nodiscard]] Job *find(PeerId peerId, MsgId topicRootId) const;
	not_null<Job*> create(JobSettings settings);
	void remove(not_null<Job*> job);

private:
	void load();
	void save();
	void saveDelayed();
	[[nodiscard]] QString path() const;

	const not_null<Main::Session*> _session;
	Slots _slots;
	std::vector<std::unique_ptr<Job>> _jobs;
	base::Timer _saveTimer;

};

} // namespace AyuMediaDownload
