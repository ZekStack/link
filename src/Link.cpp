#include "Link.h"

#include <cctype>
#include <cstring>

namespace {
constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

size_t boundedLength(const char *value) {
	return value == nullptr ? 0 : std::strlen(value);
}

char lowerAscii(char value) {
	if (value >= 'A' && value <= 'Z') {
		return static_cast<char>(value - 'A' + 'a');
	}
	return value;
}
} // namespace

const char *linkErrorCodeToString(LinkErrorCode code) {
	switch (code) {
	case LinkErrorCode::Ok:
		return "Ok";
	case LinkErrorCode::NotInitialized:
		return "NotInitialized";
	case LinkErrorCode::AlreadyInitialized:
		return "AlreadyInitialized";
	case LinkErrorCode::InvalidConfig:
		return "InvalidConfig";
	case LinkErrorCode::Stopping:
		return "Stopping";
	case LinkErrorCode::InvalidUrl:
		return "InvalidUrl";
	case LinkErrorCode::UrlTooLarge:
		return "UrlTooLarge";
	case LinkErrorCode::InvalidTimeout:
		return "InvalidTimeout";
	case LinkErrorCode::QueueFull:
		return "QueueFull";
	case LinkErrorCode::AllocationFailed:
		return "AllocationFailed";
	case LinkErrorCode::CallbackTooLarge:
		return "CallbackTooLarge";
	case LinkErrorCode::RequestTooLarge:
		return "RequestTooLarge";
	case LinkErrorCode::ResponseTooLarge:
		return "ResponseTooLarge";
	case LinkErrorCode::HeaderTooLarge:
		return "HeaderTooLarge";
	case LinkErrorCode::TooManyHeaders:
		return "TooManyHeaders";
	case LinkErrorCode::Timeout:
		return "Timeout";
	case LinkErrorCode::DnsFailed:
		return "DnsFailed";
	case LinkErrorCode::ConnectionFailed:
		return "ConnectionFailed";
	case LinkErrorCode::TlsFailed:
		return "TlsFailed";
	case LinkErrorCode::SendFailed:
		return "SendFailed";
	case LinkErrorCode::ReceiveFailed:
		return "ReceiveFailed";
	case LinkErrorCode::RedirectRejected:
		return "RedirectRejected";
	case LinkErrorCode::RedirectLimitReached:
		return "RedirectLimitReached";
	case LinkErrorCode::JsonSerializeFailed:
		return "JsonSerializeFailed";
	case LinkErrorCode::JsonParseFailed:
		return "JsonParseFailed";
	case LinkErrorCode::Cancelled:
		return "Cancelled";
	case LinkErrorCode::InternalError:
		return "InternalError";
	}
	return "Unknown";
}

const char *linkStateToString(LinkState state) {
	switch (state) {
	case LinkState::Uninitialized:
		return "Uninitialized";
	case LinkState::Starting:
		return "Starting";
	case LinkState::Running:
		return "Running";
	case LinkState::Stopping:
		return "Stopping";
	}
	return "Unknown";
}

LinkHeaders::~LinkHeaders() {
	clear();
	delete[] _entries;
	_entries = nullptr;
	_capacity = 0;
}

LinkHeaders::LinkHeaders(LinkHeaders &&other) noexcept {
	moveFrom(other);
}

LinkHeaders &LinkHeaders::operator=(LinkHeaders &&other) noexcept {
	if (this != &other) {
		clear();
		delete[] _entries;
		moveFrom(other);
	}
	return *this;
}

void LinkHeaders::configureLimits(
    size_t maxHeaderCount,
    size_t maxHeaderNameSize,
    size_t maxHeaderValueSize,
    size_t maxTotalHeaderSize
) {
	_maxHeaderCount = maxHeaderCount;
	_maxHeaderNameSize = maxHeaderNameSize;
	_maxHeaderValueSize = maxHeaderValueSize;
	_maxTotalHeaderSize = maxTotalHeaderSize;
}

LinkResult LinkHeaders::add(const char *name, const char *value) {
	LinkResult validation = validate(name, value, kInvalidIndex);
	if (!validation) {
		return validation;
	}
	LinkResult storageResult = ensureStorage();
	if (!storageResult) {
		return storageResult;
	}
	if (_count >= _capacity) {
		return LinkResult::error(LinkErrorCode::TooManyHeaders, "too many headers");
	}
	if (!copyEntry(_entries[_count], name, value)) {
		return LinkResult::error(LinkErrorCode::AllocationFailed, "header allocation failed");
	}
	_totalSize += _entries[_count].nameSize + _entries[_count].valueSize;
	_count++;
	return LinkResult::ok();
}

LinkResult LinkHeaders::set(const char *name, const char *value) {
	const int existing = findIndex(name);
	LinkResult validation =
	    validate(name, value, existing >= 0 ? static_cast<size_t>(existing) : kInvalidIndex);
	if (!validation) {
		return validation;
	}
	if (existing >= 0) {
		Entry replacement;
		if (!copyEntry(replacement, name, value)) {
			return LinkResult::error(LinkErrorCode::AllocationFailed, "header allocation failed");
		}
		_totalSize -= _entries[existing].nameSize + _entries[existing].valueSize;
		freeEntry(_entries[existing]);
		_entries[existing] = replacement;
		_totalSize += replacement.nameSize + replacement.valueSize;
		return LinkResult::ok();
	}
	return add(name, value);
}

bool LinkHeaders::has(const char *name) const {
	return findIndex(name) >= 0;
}

const char *LinkHeaders::get(const char *name) const {
	const int index = findIndex(name);
	return index >= 0 ? _entries[index].value : nullptr;
}

const char *LinkHeaders::nameAt(size_t index) const {
	return index < _count ? _entries[index].name : nullptr;
}

const char *LinkHeaders::valueAt(size_t index) const {
	return index < _count ? _entries[index].value : nullptr;
}

size_t LinkHeaders::size() const {
	return _count;
}

size_t LinkHeaders::totalSize() const {
	return _totalSize;
}

void LinkHeaders::clear() {
	if (_entries != nullptr) {
		for (size_t i = 0; i < _count; ++i) {
			freeEntry(_entries[i]);
		}
	}
	_count = 0;
	_totalSize = 0;
}

bool LinkHeaders::namesEqual(const char *left, const char *right) {
	if (left == nullptr || right == nullptr) {
		return false;
	}
	while (*left != '\0' && *right != '\0') {
		if (lowerAscii(*left) != lowerAscii(*right)) {
			return false;
		}
		left++;
		right++;
	}
	return *left == '\0' && *right == '\0';
}

bool LinkHeaders::validName(const char *name) {
	if (name == nullptr || *name == '\0') {
		return false;
	}
	for (const char *cursor = name; *cursor != '\0'; ++cursor) {
		const unsigned char value = static_cast<unsigned char>(*cursor);
		if (value <= 32 || value >= 127 || *cursor == ':') {
			return false;
		}
	}
	return true;
}

size_t LinkHeaders::safeLength(const char *value) {
	return boundedLength(value);
}

int LinkHeaders::findIndex(const char *name) const {
	for (size_t i = 0; i < _count; ++i) {
		if (namesEqual(_entries[i].name, name)) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

LinkResult LinkHeaders::ensureStorage() {
	if (_entries != nullptr) {
		return LinkResult::ok();
	}
	if (_maxHeaderCount == 0) {
		return LinkResult::error(LinkErrorCode::TooManyHeaders, "header count limit is zero");
	}
	_entries = new (std::nothrow) Entry[_maxHeaderCount];
	if (_entries == nullptr) {
		return LinkResult::error(
		    LinkErrorCode::AllocationFailed, "header storage allocation failed"
		);
	}
	_capacity = _maxHeaderCount;
	return LinkResult::ok();
}

LinkResult LinkHeaders::validate(const char *name, const char *value, size_t replacingIndex) const {
	if (!validName(name)) {
		return LinkResult::error(LinkErrorCode::HeaderTooLarge, "header name is invalid");
	}
	const size_t nameSize = safeLength(name);
	const size_t valueSize = safeLength(value);
	if (nameSize > _maxHeaderNameSize || valueSize > _maxHeaderValueSize) {
		return LinkResult::error(LinkErrorCode::HeaderTooLarge, "header field is too large");
	}
	size_t projectedTotal = _totalSize + nameSize + valueSize;
	if (replacingIndex != kInvalidIndex && replacingIndex < _count) {
		projectedTotal -= _entries[replacingIndex].nameSize + _entries[replacingIndex].valueSize;
	}
	if (projectedTotal > _maxTotalHeaderSize) {
		return LinkResult::error(LinkErrorCode::HeaderTooLarge, "headers exceed total limit");
	}
	if (replacingIndex == kInvalidIndex && _count >= _maxHeaderCount) {
		return LinkResult::error(LinkErrorCode::TooManyHeaders, "too many headers");
	}
	return LinkResult::ok();
}

bool LinkHeaders::copyEntry(Entry &entry, const char *name, const char *value) {
	const size_t nameSize = safeLength(name);
	const size_t valueSize = safeLength(value);
	entry.name = link_memory::duplicateString(name, nameSize);
	entry.value = link_memory::duplicateString(value != nullptr ? value : "", valueSize);
	if (entry.name == nullptr || entry.value == nullptr) {
		freeEntry(entry);
		return false;
	}
	entry.nameSize = nameSize;
	entry.valueSize = valueSize;
	return true;
}

void LinkHeaders::freeEntry(Entry &entry) {
	link_memory::release(entry.name);
	link_memory::release(entry.value);
	entry.name = nullptr;
	entry.value = nullptr;
	entry.nameSize = 0;
	entry.valueSize = 0;
}

void LinkHeaders::moveFrom(LinkHeaders &other) {
	_entries = other._entries;
	_count = other._count;
	_capacity = other._capacity;
	_totalSize = other._totalSize;
	_maxHeaderCount = other._maxHeaderCount;
	_maxHeaderNameSize = other._maxHeaderNameSize;
	_maxHeaderValueSize = other._maxHeaderValueSize;
	_maxTotalHeaderSize = other._maxTotalHeaderSize;

	other._entries = nullptr;
	other._count = 0;
	other._capacity = 0;
	other._totalSize = 0;
}

LinkResult LinkHeaders::copyFrom(const LinkHeaders &other) {
	if (this == &other) {
		return LinkResult::ok();
	}
	clear();
	delete[] _entries;
	_entries = nullptr;
	_capacity = 0;
	configureLimits(
	    other._maxHeaderCount,
	    other._maxHeaderNameSize,
	    other._maxHeaderValueSize,
	    other._maxTotalHeaderSize
	);
	if (other._count == 0) {
		return LinkResult::ok();
	}
	LinkResult storageResult = ensureStorage();
	if (!storageResult) {
		return storageResult;
	}
	for (size_t i = 0; i < other._count; ++i) {
		LinkResult addResult = add(other._entries[i].name, other._entries[i].value);
		if (!addResult) {
			clear();
			return addResult;
		}
	}
	return LinkResult::ok();
}

LinkBodyView LinkBodyView::none() {
	return LinkBodyView{};
}

LinkBodyView LinkBodyView::text(const char *value) {
	LinkBodyView body;
	body._type = LinkBodyType::Text;
	body._data = reinterpret_cast<const uint8_t *>(value != nullptr ? value : "");
	body._size = boundedLength(value);
	return body;
}

LinkBodyView LinkBodyView::json(const JsonDocument &json) {
	LinkBodyView body;
	body._type = LinkBodyType::Json;
	body._json = &json;
	return body;
}

LinkBodyView LinkBodyView::bytes(const uint8_t *data, size_t size) {
	LinkBodyView body;
	body._type = LinkBodyType::Binary;
	body._data = data;
	body._size = size;
	body._valid = data != nullptr || size == 0;
	return body;
}

LinkBodyType LinkBodyView::type() const {
	return _type;
}

const uint8_t *LinkBodyView::data() const {
	return _data;
}

const JsonDocument *LinkBodyView::jsonDocument() const {
	return _json;
}

size_t LinkBodyView::size() const {
	return _size;
}

bool LinkBodyView::valid() const {
	return _valid;
}

LinkBodyType LinkBody::type() const {
	return _type;
}

const uint8_t *LinkBody::data() const {
	return _buffer.data();
}

const char *LinkBody::c_str() const {
	return _buffer.c_str();
}

size_t LinkBody::size() const {
	return _buffer.size();
}

LinkErrorCode LinkBody::status() const {
	return _status;
}

bool LinkBody::valid() const {
	return _status == LinkErrorCode::Ok;
}

void LinkBody::clear() {
	_type = LinkBodyType::None;
	_buffer.clear();
	_status = LinkErrorCode::Ok;
}

LinkResult LinkBody::copyFrom(const LinkBody &other) {
	if (this == &other) {
		return LinkResult::ok();
	}
	clear();
	_type = other._type;
	_status = other._status;
	if (other._status != LinkErrorCode::Ok) {
		return LinkResult::error(other._status, "request body is invalid");
	}
	if (!_buffer.copyFrom(other._buffer)) {
		_type = LinkBodyType::None;
		_status = LinkErrorCode::AllocationFailed;
		return LinkResult::error(LinkErrorCode::AllocationFailed, "body allocation failed");
	}
	return LinkResult::ok();
}

namespace link_internal {

bool linkUrlLooksValid(const char *url) {
	if (url == nullptr) {
		return false;
	}
	return std::strncmp(url, "http://", 7) == 0 || std::strncmp(url, "https://", 8) == 0;
}

LinkResult linkBodyFromView(const LinkBodyView &view, const LinkConfig &config, LinkBody &out) {
	if (!view.valid()) {
		return LinkResult::error(LinkErrorCode::InternalError, "request body view is invalid");
	}

	size_t size = view.size();
	const JsonDocument *json = view.jsonDocument();
	if (view.type() == LinkBodyType::Json) {
		if (json == nullptr) {
			return LinkResult::error(LinkErrorCode::InternalError, "json body view is invalid");
		}
		size = measureJson(*json);
		if (size > config.maxSerializedJsonSize) {
			return LinkResult::error(LinkErrorCode::RequestTooLarge, "json body is too large");
		}
	}
	if (size > config.maxRequestBodySize) {
		return LinkResult::error(LinkErrorCode::RequestTooLarge, "request body is too large");
	}

	out.clear();
	out._type = view.type();
	bool allocated = true;
	switch (view.type()) {
	case LinkBodyType::None:
		break;
	case LinkBodyType::Text:
		allocated = out._buffer.assignText(reinterpret_cast<const char *>(view.data()), size);
		break;
	case LinkBodyType::Binary:
		allocated = out._buffer.assign(view.data(), size);
		break;
	case LinkBodyType::Json:
		allocated = out._buffer.allocateForWrite(size, true);
		if (allocated) {
			const size_t written = serializeJson(*json, out._buffer.c_str(), size + 1);
			if (written != size) {
				out.clear();
				return LinkResult::error(
				    LinkErrorCode::JsonSerializeFailed, "json serialization failed"
				);
			}
		}
		break;
	}
	if (!allocated) {
		out.clear();
		return LinkResult::error(LinkErrorCode::AllocationFailed, "body allocation failed");
	}
	return LinkResult::ok();
}

} // namespace link_internal
