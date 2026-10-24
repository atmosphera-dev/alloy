// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/secret_service_key_source.h"

#include <cstdlib>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/memory/scoped_refptr.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "third_party/boringssl/src/include/openssl/crypto.h"

namespace alloy::site_records {
namespace {
constexpr char kService[] = "org.freedesktop.secrets";
constexpr char kPath[] = "/org/freedesktop/secrets";
constexpr char kInterface[] = "org.freedesktop.Secret.Service";
constexpr char kCollection[] = "org.freedesktop.Secret.Collection";
constexpr char kItem[] = "org.freedesktop.Secret.Item";
constexpr char kSchema[] = "org.alloy.ProfileKey.v1";
constexpr int kTimeoutMs = 3000;

// libdbus owns the serialized byte arrays (including malformed replies). Wipe
// in place after the synchronous send/read has finished, before unref/free.
// No iterator writes, container sizes or message structure are changed.
void WipeArrays(DBusMessageIter* iter) {
  do {
    const int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_INVALID) return;
    if (type == DBUS_TYPE_ARRAY &&
        dbus_message_iter_get_element_type(iter) == DBUS_TYPE_BYTE) {
      DBusMessageIter child;
      dbus_message_iter_recurse(iter, &child);
      void* bytes = nullptr;
      int length = 0;
      dbus_message_iter_get_fixed_array(&child, &bytes, &length);
      if (length > 0) OPENSSL_cleanse(bytes, length);
    } else if (type == DBUS_TYPE_ARRAY || type == DBUS_TYPE_STRUCT ||
               type == DBUS_TYPE_DICT_ENTRY || type == DBUS_TYPE_VARIANT) {
      DBusMessageIter child;
      dbus_message_iter_recurse(iter, &child);
      WipeArrays(&child);
    }
  } while (dbus_message_iter_next(iter));
}

class WipeMessage {
 public:
  explicit WipeMessage(dbus::Message* message) : message_(message) {}
  ~WipeMessage() {
    DBusMessageIter iter;
    if (message_ && dbus_message_iter_init(message_->raw_message(), &iter))
      WipeArrays(&iter);
  }
 private:
  dbus::Message* message_;
};

void Attributes(dbus::MessageWriter* writer, const ObjectId& salt) {
  dbus::MessageWriter array(nullptr);
  writer->OpenArray("{ss}", &array);
  for (const auto& [name, value] :
       {std::pair<std::string, std::string>{"application", "alloy"},
        {"xdg:schema", kSchema}, {"profile", ObjectName(salt)}}) {
    dbus::MessageWriter entry(nullptr);
    array.OpenDictEntry(&entry);
    entry.AppendString(name);
    entry.AppendString(value);
    array.CloseContainer(&entry);
  }
  writer->CloseContainer(&array);
}

class Operation {
 public:
  Operation() : bus_(base::MakeRefCounted<dbus::Bus>(dbus::Bus::Options())) {}
  ~Operation() {
    if (!session_.value().empty()) {
      dbus::MethodCall close("org.freedesktop.Secret.Session", "Close");
      Call(session_, &close);
    }
    bus_->ShutdownAndBlock();
  }
  bool Connect() {
    // libdbus may otherwise autolaunch a session bus through the desktop.
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address || !std::string_view(address).starts_with("unix:") ||
        std::string_view(address).find(';') != std::string_view::npos ||
        !bus_->Connect()) return false;
    dbus::MethodCall owner("org.freedesktop.DBus", "GetNameOwner");
    dbus::MessageWriter(&owner).AppendString(kService);
    owner.SetDestination("org.freedesktop.DBus");
    owner.SetPath(dbus::ObjectPath("/org/freedesktop/DBus"));
    dbus_message_set_auto_start(owner.raw_message(), false);
    auto response = bus_->SendWithReplyAndBlock(owner.raw_message(), kTimeoutMs);
    if (!response.has_value()) return false;
    dbus::MessageReader reader(response->get());
    return reader.PopString(&owner_) && !reader.HasMoreData() &&
           owner_.starts_with(":");
  }
  std::unique_ptr<dbus::Response> Call(const dbus::ObjectPath& path,
                                       dbus::MethodCall* call) {
    call->SetDestination(owner_);  // Pin this owner throughout the operation.
    call->SetPath(path);
    dbus_message_set_auto_start(call->raw_message(), false);
    auto response = bus_->SendWithReplyAndBlock(call->raw_message(), kTimeoutMs);
    return response.has_value() ? std::move(response.value()) : nullptr;
  }
  KeyringResult Run(const ObjectId& salt, bool create, SecretKey* output) {
    if (!Connect()) return KeyringResult::kUnavailable;
    dbus::MethodCall alias(kInterface, "ReadAlias");
    dbus::MessageWriter(&alias).AppendString("default");
    auto response = Call(dbus::ObjectPath(kPath), &alias);
    if (!response) return KeyringResult::kFailed;
    dbus::MessageReader alias_reader(response.get());
    dbus::ObjectPath collection;
    if (!alias_reader.PopObjectPath(&collection) || alias_reader.HasMoreData())
      return KeyringResult::kFailed;
    if (collection.value() == "/") return KeyringResult::kMissing;
    auto locked = Locked(collection, kCollection);
    if (locked != KeyringResult::kOk) return locked;

    dbus::MethodCall search(kCollection, "SearchItems");
    dbus::MessageWriter search_writer(&search);
    Attributes(&search_writer, salt);
    response = Call(collection, &search);
    if (!response) return KeyringResult::kFailed;
    dbus::MessageReader search_reader(response.get());
    std::vector<dbus::ObjectPath> items;
    if (!search_reader.PopArrayOfObjectPaths(&items) || search_reader.HasMoreData() ||
        items.size() > 1) return KeyringResult::kFailed;
    if (create ? !items.empty() : items.empty())
      return create ? KeyringResult::kFailed : KeyringResult::kMissing;
    if (!create) {
      locked = Locked(items.front(), kItem);
      if (locked != KeyringResult::kOk) return locked;
    }

    dbus::MethodCall open(kInterface, "OpenSession");
    dbus::MessageWriter open_writer(&open);
    open_writer.AppendString("plain");
    open_writer.AppendVariantOfString("");
    response = Call(dbus::ObjectPath(kPath), &open);
    if (!response) return KeyringResult::kFailed;
    dbus::MessageReader open_reader(response.get());
    std::string negotiated;
    if (!open_reader.PopVariantOfString(&negotiated) || !negotiated.empty() ||
        !open_reader.PopObjectPath(&session_) || open_reader.HasMoreData() ||
        session_.value() == "/") return KeyringResult::kFailed;
    return create ? Create(collection, salt, output) : Read(items.front(), output);
  }
 private:
  KeyringResult Locked(const dbus::ObjectPath& path, const std::string& interface) {
    dbus::MethodCall get("org.freedesktop.DBus.Properties", "Get");
    dbus::MessageWriter writer(&get);
    writer.AppendString(interface);
    writer.AppendString("Locked");
    auto response = Call(path, &get);
    if (!response) return KeyringResult::kFailed;
    dbus::MessageReader reader(response.get());
    bool locked = true;
    if (!reader.PopVariantOfBool(&locked) || reader.HasMoreData())
      return KeyringResult::kFailed;
    return locked ? KeyringResult::kLocked : KeyringResult::kOk;
  }
  KeyringResult Create(const dbus::ObjectPath& collection, const ObjectId& salt,
                        SecretKey* output) {
    SecretKey generated;
    if (RandomKey(&generated) != FileStatus::kOk) return KeyringResult::kFailed;
    dbus::MethodCall create(kCollection, "CreateItem");
    WipeMessage wipe(&create);
    dbus::MessageWriter writer(&create), properties(nullptr), entry(nullptr), variant(nullptr);
    writer.OpenArray("{sv}", &properties);
    properties.OpenDictEntry(&entry);
    entry.AppendString(std::string(kItem) + ".Label");
    entry.AppendVariantOfString("Alloy profile key");
    properties.CloseContainer(&entry);
    properties.OpenDictEntry(&entry);
    entry.AppendString(std::string(kItem) + ".Attributes");
    entry.OpenVariant("a{ss}", &variant);
    Attributes(&variant, salt);
    entry.CloseContainer(&variant);
    properties.CloseContainer(&entry);
    writer.CloseContainer(&properties);
    dbus::MessageWriter secret(nullptr);
    writer.OpenStruct(&secret);
    secret.AppendObjectPath(session_);
    secret.AppendArrayOfBytes({});
    secret.AppendArrayOfBytes(generated.bytes());
    secret.AppendString("application/octet-stream");
    writer.CloseContainer(&secret);
    writer.AppendBool(false);  // Never replace an existing key.
    auto response = Call(collection, &create);
    if (!response) return KeyringResult::kFailed;
    dbus::MessageReader reader(response.get());
    dbus::ObjectPath item, prompt;
    if (!reader.PopObjectPath(&item) || !reader.PopObjectPath(&prompt) ||
        reader.HasMoreData() || item.value() == "/" || prompt.value() != "/")
      return KeyringResult::kFailed;
    // Read back what the provider committed, rather than trusting a write ack.
    auto result = Read(item, output);
    if (result == KeyringResult::kOk &&
        CRYPTO_memcmp(generated.bytes().data(), output->bytes().data(), 32) != 0) {
      output->Clear();
      return KeyringResult::kFailed;
    }
    return result;
  }
  KeyringResult Read(const dbus::ObjectPath& item, SecretKey* output) {
    dbus::MethodCall get(kItem, "GetSecret");
    dbus::MessageWriter(&get).AppendObjectPath(session_);
    auto response = Call(item, &get);
    if (!response) return KeyringResult::kFailed;
    WipeMessage wipe(response.get());
    dbus::MessageReader reader(response.get()), secret(nullptr);
    dbus::ObjectPath session;
    base::span<const uint8_t> parameters, value;
    std::string content_type;
    if (!reader.PopStruct(&secret) || reader.HasMoreData() ||
        !secret.PopObjectPath(&session) || session != session_ ||
        !secret.PopArrayOfBytes(&parameters) || !parameters.empty() ||
        !secret.PopArrayOfBytes(&value) || value.size() != 32 ||
        !secret.PopString(&content_type) ||
        secret.HasMoreData()) return KeyringResult::kFailed;
    // ContentType is descriptive, not the secret's type or authentication.
    // GNOME normalizes it to text/plain even for binary CreateItem input. The
    // actual contract is a session-bound, exact 32-byte value; creation also
    // constant-time compares the readback with the generated key.
    *output = SecretKey(value.first<32>());
    return KeyringResult::kOk;
  }
  scoped_refptr<dbus::Bus> bus_;
  std::string owner_;
  dbus::ObjectPath session_;
};
}  // namespace

KeySourceKind SecretServiceKeySource::kind() const { return KeySourceKind::kSecretService; }
FileStatus SecretServiceKeySource::Create(const KeySourceParameters& p, SecretKey* key) {
  return Get(p, true, key);
}
FileStatus SecretServiceKeySource::Derive(const KeySourceParameters& p, SecretKey* key) {
  return Get(p, false, key);
}
FileStatus SecretServiceKeySource::Get(const KeySourceParameters& p, bool create,
                                       SecretKey* key) {
  key->Clear();
  result_ = KeyringResult::kFailed;
  if (p.kind != kind()) return FileStatus::kDenied;
  Operation operation;
  result_ = operation.Run(p.salt, create, key);
  if (result_ != KeyringResult::kOk) key->Clear();
  return result_ == KeyringResult::kOk ? FileStatus::kOk : FileStatus::kDenied;
}
}  // namespace alloy::site_records
