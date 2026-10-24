// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_TREE_H_
#define BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_TREE_H_

#include <functional>
#include <set>
#include <vector>

#include "brave/alloy/site_records/authenticated_objects.h"

namespace alloy::site_records {

// Immutable, path-compressed binary Merkle trie. Each 4 KiB encrypted page is
// either a key/value leaf or a branch at the first differing key bit. Every
// parent authenticates child IDs/digests AND the prefix/branch semantic bounds.
// Updates copy only the search path (at most 256 branches), not an aggregate
// namespace/catalogue. HMAC keys give expected logarithmic depth for names.
// Numeric chunk keys keep their natural sparse ordering without allocating
// holes. No plaintext page cache; temporary decoded nodes are wiped.
class EncryptedTree {
 public:
  static constexpr size_t kMaxValueSize = 3896;
  using Visitor =
      std::function<FileStatus(const ObjectId&, base::span<const uint8_t>)>;

  EncryptedTree(ObjectDirectory& directory,
                ObjectType type,
                const SecretKey& key,
                base::span<const uint8_t> binding);
  ~EncryptedTree();
  EncryptedTree(const EncryptedTree&) = delete;
  EncryptedTree& operator=(const EncryptedTree&) = delete;

  FileStatus Get(const ObjectRef& root,
                 const ObjectId& key,
                 std::vector<uint8_t>* value,
                 ObjectRef* authenticated_leaf = nullptr) const;
  FileStatus Put(const ObjectRef& root,
                 const ObjectId& key,
                 base::span<const uint8_t> value,
                 ObjectRef* result);
  FileStatus Erase(const ObjectRef& root,
                   const ObjectId& key,
                   ObjectRef* result);
  FileStatus Visit(const ObjectRef& root, const Visitor& visitor) const;
  // Traversal validates every page, not just directory names. Required before
  // unlinking superseded key-bearing catalogue pages after publication.
  FileStatus Reachable(const ObjectRef& root,
                       std::set<ObjectId>* objects) const;

 private:
  struct Node;
  struct Bound;
  FileStatus Load(const ObjectRef& ref, const Bound& bound, Node* node) const;
  FileStatus Store(const Node& node, ObjectRef* result);
  FileStatus PutAt(const ObjectRef& ref,
                   const Bound& bound,
                   const ObjectId& key,
                   base::span<const uint8_t> value,
                   ObjectRef* result);
  FileStatus EraseAt(const ObjectRef& ref,
                     const Bound& bound,
                     const ObjectId& key,
                     ObjectRef* result);
  FileStatus Walk(const ObjectRef& ref,
                  const Bound& bound,
                  std::set<ObjectId>* seen,
                  const Visitor& visitor) const;

  ObjectDirectory& directory_;
  const ObjectType type_;
  const SecretKey& key_;
  const std::vector<uint8_t> binding_;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_TREE_H_
