// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_tree.h"

#include <algorithm>
#include <utility>

#include "base/check.h"

namespace alloy::site_records {
namespace {

// A fixed page header: version, leaf flag, branch bit, key/prefix, two child
// refs, value length, reserved zero bytes. Values follow at offset 200.
constexpr size_t kValueOffset = 200;

bool Bit(const ObjectId& key, uint16_t bit) {
  CHECK_LT(bit, 256);
  return (base::span(key)[bit / 8] >> (7 - bit % 8)) & 1;
}

uint16_t CommonBits(const ObjectId& a, const ObjectId& b) {
  uint16_t bit = 0;
  while (bit < 256 && Bit(a, bit) == Bit(b, bit)) {
    ++bit;
  }
  return bit;
}

bool ValidRef(const ObjectRef& ref) {
  return (ref.id == ObjectId{}) == (ref.digest == ObjectId{});
}

void WriteRef(base::span<uint8_t, 64> bytes, const ObjectRef& ref) {
  bytes.first<32>().copy_from(ref.id);
  bytes.last<32>().copy_from(ref.digest);
}

ObjectRef ReadRef(base::span<const uint8_t, 64> bytes) {
  ObjectRef ref;
  base::span(ref.id).copy_from(bytes.first<32>());
  base::span(ref.digest).copy_from(bytes.last<32>());
  return ref;
}

}  // namespace

struct EncryptedTree::Node {
  Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  ~Node() {
    Cleanse(value);
    Cleanse(key);
  }
  bool leaf = true;
  uint16_t bit = 256;
  ObjectId key{};
  ObjectRef left;
  ObjectRef right;
  std::vector<uint8_t> value;
};

struct EncryptedTree::Bound {
  ObjectId prefix{};
  // Root has no parent constraint. Children must extend this prefix through
  // the parent's branch bit, and their own branch bit must strictly increase.
  int bit = -1;
  bool right = false;

  bool Accepts(const Node& node) const {
    return bit < 0 ||
           (node.bit > bit && CommonBits(prefix, node.key) >= bit &&
            Bit(node.key, bit) == right);
  }
};

EncryptedTree::EncryptedTree(ObjectDirectory& directory,
                              ObjectType type,
                              const SecretKey& key,
                              base::span<const uint8_t> binding)
    : directory_(directory),
      type_(type),
      key_(key),
      binding_(binding.begin(), binding.end()) {
  CHECK(type == ObjectType::kCatalogue || type == ObjectType::kNamespace ||
        type == ObjectType::kChunkMap);
}
EncryptedTree::~EncryptedTree() = default;

FileStatus EncryptedTree::Load(const ObjectRef& ref,
                                const Bound& bound,
                                Node* node) const {
  if (ref.empty() || !ValidRef(ref)) {
    return FileStatus::kIntegrity;
  }
  ObjectPage page;
  auto status = directory_.Read(type_, key_, binding_, 0, ref, &page);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto bytes = base::span(page);
  node->leaf = bytes[1] == 1;
  node->bit = (static_cast<uint16_t>(bytes[2]) << 8) | bytes[3];
  base::span(node->key).copy_from(bytes.subspan<4, 32>());
  node->left = ReadRef(bytes.subspan<36, 64>());
  node->right = ReadRef(bytes.subspan<100, 64>());
  const size_t length = (static_cast<size_t>(bytes[164]) << 8) | bytes[165];
  if (bytes[0] != 1 || bytes[1] > 1 || length > kMaxValueSize ||
      !ValidRef(node->left) || !ValidRef(node->right) ||
      !std::ranges::all_of(bytes.subspan(166u, kValueOffset - 166),
                           [](uint8_t b) { return b == 0; }) ||
      !std::ranges::all_of(bytes.subspan(kValueOffset + length),
                           [](uint8_t b) { return b == 0; }) ||
      (node->leaf && (node->bit != 256 || !node->left.empty() ||
                       !node->right.empty())) ||
      (!node->leaf && (node->bit >= 256 || length != 0 ||
                        node->left.empty() || node->right.empty() ||
                        node->left.id == node->right.id)) ||
      !bound.Accepts(*node)) {
    status = FileStatus::kIntegrity;
  } else {
    const auto value = bytes.subspan(kValueOffset, length);
    node->value.assign(value.begin(), value.end());
  }
  Cleanse(page);
  return status;
}

FileStatus EncryptedTree::Store(const Node& node, ObjectRef* result) {
  ObjectPage page{};
  auto bytes = base::span(page);
  bytes[0] = 1;
  bytes[1] = node.leaf;
  bytes[2] = node.bit >> 8;
  bytes[3] = node.bit & 255;
  bytes.subspan<4, 32>().copy_from(node.key);
  WriteRef(bytes.subspan<36, 64>(), node.left);
  WriteRef(bytes.subspan<100, 64>(), node.right);
  bytes[164] = node.value.size() >> 8;
  bytes[165] = node.value.size() & 255;
  bytes.subspan(kValueOffset, node.value.size()).copy_from(node.value);
  const auto status = directory_.Seal(type_, key_, binding_, 0, page, result);
  Cleanse(page);
  return status;
}

FileStatus EncryptedTree::Get(const ObjectRef& root,
                               const ObjectId& key,
                               std::vector<uint8_t>* value) const {
  auto ref = root;
  Bound bound;
  while (!ref.empty()) {
    Node node;
    const auto status = Load(ref, bound, &node);
    if (status != FileStatus::kOk) {
      return status;
    }
    if (CommonBits(node.key, key) < node.bit) {
      return FileStatus::kNotFound;
    }
    if (node.leaf) {
      Cleanse(*value);
      *value = std::move(node.value);
      return FileStatus::kOk;
    }
    const bool right = Bit(key, node.bit);
    bound = {node.key, node.bit, right};
    ref = right ? node.right : node.left;
  }
  return ValidRef(ref) ? FileStatus::kNotFound : FileStatus::kIntegrity;
}

FileStatus EncryptedTree::Put(const ObjectRef& root,
                               const ObjectId& key,
                               base::span<const uint8_t> value,
                               ObjectRef* result) {
  if (value.size() > kMaxValueSize) {
    return FileStatus::kInvalid;
  }
  return PutAt(root, Bound(), key, value, result);
}

FileStatus EncryptedTree::PutAt(const ObjectRef& ref,
                                 const Bound& bound,
                                 const ObjectId& key,
                                 base::span<const uint8_t> value,
                                 ObjectRef* result) {
  if (!ValidRef(ref)) {
    return FileStatus::kIntegrity;
  }
  Node node;
  if (ref.empty()) {
    node.key = key;
    node.value.assign(value.begin(), value.end());
    return Store(node, result);
  }
  auto status = Load(ref, bound, &node);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto common = CommonBits(node.key, key);
  if (common < node.bit) {
    Node leaf;
    leaf.key = key;
    leaf.value.assign(value.begin(), value.end());
    ObjectRef added;
    status = Store(leaf, &added);
    if (status != FileStatus::kOk) {
      return status;
    }
    Node branch;
    branch.leaf = false;
    branch.bit = common;
    branch.key = key;
    branch.left = Bit(key, common) ? ref : added;
    branch.right = Bit(key, common) ? added : ref;
    return Store(branch, result);
  }
  if (node.leaf) {
    Cleanse(node.value);
    node.value.assign(value.begin(), value.end());
  } else {
    const bool right = Bit(key, node.bit);
    auto& child = right ? node.right : node.left;
    ObjectRef updated;
    status = PutAt(child, {node.key, node.bit, right}, key, value, &updated);
    if (status != FileStatus::kOk) {
      return status;
    }
    child = updated;
  }
  return Store(node, result);
}

FileStatus EncryptedTree::Erase(const ObjectRef& root,
                                 const ObjectId& key,
                                 ObjectRef* result) {
  return EraseAt(root, Bound(), key, result);
}

FileStatus EncryptedTree::EraseAt(const ObjectRef& ref,
                                   const Bound& bound,
                                   const ObjectId& key,
                                   ObjectRef* result) {
  if (!ValidRef(ref)) {
    return FileStatus::kIntegrity;
  }
  if (ref.empty()) {
    return FileStatus::kNotFound;
  }
  Node node;
  auto status = Load(ref, bound, &node);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (CommonBits(node.key, key) < node.bit) {
    return FileStatus::kNotFound;
  }
  if (node.leaf) {
    *result = ObjectRef{};
    return FileStatus::kOk;
  }
  const bool right = Bit(key, node.bit);
  auto& child = right ? node.right : node.left;
  ObjectRef updated;
  status = EraseAt(child, {node.key, node.bit, right}, key, &updated);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (updated.empty()) {
    *result = right ? node.left : node.right;
    return FileStatus::kOk;
  }
  child = updated;
  return Store(node, result);
}

FileStatus EncryptedTree::Walk(const ObjectRef& ref,
                                const Bound& bound,
                                std::set<ObjectId>* seen,
                                const Visitor& visitor) const {
  if (!ValidRef(ref)) {
    return FileStatus::kIntegrity;
  }
  if (ref.empty()) {
    return FileStatus::kOk;
  }
  if (!seen->insert(ref.id).second) {
    return FileStatus::kIntegrity;
  }
  Node node;
  auto status = Load(ref, bound, &node);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (node.leaf) {
    return visitor ? visitor(node.key, node.value) : FileStatus::kOk;
  }
  status = Walk(node.left, {node.key, node.bit, false}, seen, visitor);
  return status == FileStatus::kOk
             ? Walk(node.right, {node.key, node.bit, true}, seen, visitor)
             : status;
}

FileStatus EncryptedTree::Visit(const ObjectRef& root,
                                 const Visitor& visitor) const {
  std::set<ObjectId> seen;
  return Walk(root, Bound(), &seen, visitor);
}

FileStatus EncryptedTree::Reachable(const ObjectRef& root,
                                     std::set<ObjectId>* objects) const {
  return Walk(root, Bound(), objects, Visitor());
}

}  // namespace alloy::site_records
