// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_filter_preferences.h"
#include <atomic>
#include <map>
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/strings/string_number_conversions.h"
#include "base/unguessable_token.h"
#include "crypto/sha2.h"
namespace alloy::site_records {
namespace {
using Provider=brave_shields::AdBlockSubscriptionFiltersProvider;
using Manager=brave_shields::AdBlockSubscriptionServiceManager;
using Result=Provider::SourceResult;
std::string Name(const GURL& url) { return "subscription-"+base::HexEncode(crypto::SHA256HashString(url.spec())); }
Result Status(FileStatus status) {
  return status==FileStatus::kOk ? Result::kOk : status==FileStatus::kNotFound ? Result::kMissing :
      status==FileStatus::kDenied || status==FileStatus::kCancelled ? Result::kCancelled : Result::kError;
}
struct Epoch { std::atomic<bool> active{true}; base::OnceClosure cancel; };
struct Entry {
  std::atomic<bool> active{true}; std::shared_ptr<Epoch> request;
  const std::string source_path="subscription-source/"+base::UnguessableToken::Create().ToString()+".html";
};
struct Bytes {
  Bytes()=default;Bytes(Bytes&&)=default;~Bytes(){Cleanse(value);}
  FileStatus status=FileStatus::kOk;DATFileDataBuffer value;
};
class Subscriptions final : public base::RefCountedDeleteOnSequence<Subscriptions> {
 public:
  Subscriptions(std::shared_ptr<ProfileFilterStorage> storage,
       scoped_refptr<base::SequencedTaskRunner> io, ProfileFilterPreferences::SubscriptionFetch fetch,
      base::RepeatingCallback<void(FileStatus)> error,base::RepeatingCallback<bool()> admitted)
      : base::RefCountedDeleteOnSequence<Subscriptions>(base::SequencedTaskRunner::GetCurrentDefault()),
        storage_(std::move(storage)),io_(std::move(io)),fetch_(std::move(fetch)),error_(std::move(error)),admitted_(std::move(admitted)) {}
  bool Available() const {return !closed_&&storage_->active()&&admitted_.Run();}
  std::shared_ptr<Entry> Scope(const GURL& url) {
    auto& entry=entries_[url];if(!entry)entry=std::make_shared<Entry>();return entry;
  }
  GURL SourceUrl(const GURL& url) {
    if(!Available() || storage_->mode()==StorageMode::kStateless)return GURL();
    return GURL("chrome://adblock/"+Scope(url)->source_path);
  }
  void ReadView(const std::string& path,Provider::SourceReply reply) {
    if(Available()) {
      for(const auto& [url,entry]:entries_) {
        if(entry->source_path==path && entry->active.load()) {
          Source(url).read.Run(std::move(reply));return;
        }
      }
    }
    std::move(reply).Run(Result::kCancelled,{});
  }
  Provider::Source Source(const GURL& url) {
    if(!Available() || storage_->mode()==StorageMode::kStateless) {
      Provider::Source denied;
      denied.admitted=base::BindRepeating([]{return false;});
      denied.read=base::BindRepeating([](Provider::SourceReply reply){std::move(reply).Run(Result::kCancelled,{});});
      return denied;
    }
    auto entry=Scope(url);auto storage=storage_;Provider::Source source;
    source.admitted=base::BindRepeating([](scoped_refptr<Subscriptions> self,std::shared_ptr<Entry> entry){
      return self->Available()&&entry->active.load(std::memory_order_acquire);
    },base::WrapRefCounted(this),entry);
    source.read=base::BindRepeating([](scoped_refptr<Subscriptions> self,GURL url,std::shared_ptr<Entry> entry,Provider::SourceReply reply){
      if(!self->Available()||!entry->active.load()){std::move(reply).Run(Result::kCancelled,{});return;}
      auto completion=std::make_shared<Provider::SourceReply>(std::move(reply));
      const bool posted=self->io_->PostTaskAndReplyWithResult(FROM_HERE,base::BindOnce(
          [](std::shared_ptr<ProfileFilterStorage> storage,GURL url,std::shared_ptr<Entry> entry){
            Bytes result;
            if(!entry->active.load(std::memory_order_acquire))result.status=FileStatus::kCancelled;
            else result.status=storage->Read(Name(url),&result.value);
            return result;
          },self->storage_,url,entry),base::BindOnce(
              [](scoped_refptr<Subscriptions> self,std::shared_ptr<Entry> entry,std::shared_ptr<Provider::SourceReply> completion,Bytes result){
                if((result.status==FileStatus::kOk||result.status==FileStatus::kNotFound)&&
                    (!self->Available()||!entry->active.load(std::memory_order_acquire)))result.status=FileStatus::kCancelled;
                if(result.status!=FileStatus::kOk)Cleanse(result.value);
                self->Error(result.status);std::move(*completion).Run(Status(result.status),std::move(result.value));
              },self,entry,completion));
      if(!posted)std::move(*completion).Run(Result::kCancelled,{});
    },base::WrapRefCounted(this),url,entry);
    return source;
  }
  void Download(const GURL& url,bool from_ui,base::OnceCallback<void(Result)> done) {
    scoped_refptr<Subscriptions> alive(this);
    if(!Available() || storage_->mode()==StorageMode::kStateless) { std::move(done).Run(Result::kCancelled);return; }
    auto entry=Scope(url);
    if(entry->request){entry->request->active.store(false);if(entry->request->cancel)std::move(entry->request->cancel).Run();}
    if(!Available()||!entry->active.load()){std::move(done).Run(Result::kCancelled);return;}
    auto epoch=std::make_shared<Epoch>();entry->request=epoch;
    auto cancel=fetch_.Run(url,from_ui,base::BindOnce(
        [](scoped_refptr<Subscriptions> self,GURL url,std::shared_ptr<Entry> entry,std::shared_ptr<Epoch> epoch,
           base::OnceCallback<void(Result)> done,Result status,DATFileDataBuffer bytes){
          if(!self->Active(entry,epoch)){Cleanse(bytes);std::move(done).Run(Result::kCancelled);return;}
          if(status!=Result::kOk){Cleanse(bytes);epoch->active.store(false);std::move(done).Run(status);return;}
          Bytes owned;owned.value=std::move(bytes);auto completion=std::make_shared<base::OnceCallback<void(Result)>>(std::move(done));
          const bool posted=self->io_->PostTaskAndReplyWithResult(FROM_HERE,base::BindOnce(
              [](std::shared_ptr<ProfileFilterStorage> storage,GURL url,std::shared_ptr<Entry> entry,std::shared_ptr<Epoch> epoch,Bytes bytes){
                if(!entry->active.load()||!epoch->active.load())return FileStatus::kCancelled;
                return storage->Write(Name(url),bytes.value);
              },self->storage_,url,entry,epoch,std::move(owned)),base::BindOnce(
                  [](scoped_refptr<Subscriptions> self,std::shared_ptr<Entry> entry,std::shared_ptr<Epoch> epoch,
                     std::shared_ptr<base::OnceCallback<void(Result)>> completion,FileStatus status){
                    if(status==FileStatus::kOk&&!self->Active(entry,epoch))status=FileStatus::kCancelled;
                    epoch->active.store(false);self->Error(status);std::move(*completion).Run(Status(status));
                  },self,entry,epoch,completion));
          if(!posted)std::move(*completion).Run(Result::kCancelled);
        },base::WrapRefCounted(this),url,entry,epoch,std::move(done)));
    if(!Active(entry,epoch)){if(cancel)std::move(cancel).Run();}else epoch->cancel=std::move(cancel);
  }
  void Cancel(const GURL& url) {
    auto found=entries_.find(url);if(found==entries_.end())return;
    auto entry=found->second;entry->active.store(false);entries_.erase(found);
    if(entry->request){entry->request->active.store(false);if(entry->request->cancel)std::move(entry->request->cancel).Run();}
  }
  bool Remove(const GURL& url) {
    if(!Available()||storage_->mode()==StorageMode::kStateless)return false;
    Cancel(url);return io_->PostTaskAndReplyWithResult(FROM_HERE,base::BindOnce(
        [](std::shared_ptr<ProfileFilterStorage> storage,GURL url){return storage->Remove(Name(url));},storage_,url),
        base::BindOnce([](scoped_refptr<Subscriptions> self,FileStatus status){self->Error(status);},base::WrapRefCounted(this)));
  }
  void Close() {
    if(closed_)return;
    scoped_refptr<Subscriptions> alive(this);closed_=true;
    while(!entries_.empty())Cancel(entries_.begin()->first);
  }
  base::WeakPtr<Subscriptions> GetWeakPtr(){return weak_.GetWeakPtr();}
  void Observe(base::CallbackListSubscription subscription){retirement_=std::move(subscription);}
 private:
  friend class base::RefCountedDeleteOnSequence<Subscriptions>;
  friend class base::DeleteHelper<Subscriptions>;
  ~Subscriptions(){closed_=true;while(!entries_.empty())Cancel(entries_.begin()->first);}
  bool Active(const std::shared_ptr<Entry>& entry,const std::shared_ptr<Epoch>& epoch)const{
    return Available()&&entry->active.load()&&epoch->active.load();
  }
  void Error(FileStatus status){if(status!=FileStatus::kOk&&status!=FileStatus::kNotFound&&status!=FileStatus::kDenied&&status!=FileStatus::kCancelled)error_.Run(status);}
  std::shared_ptr<ProfileFilterStorage> storage_;scoped_refptr<base::SequencedTaskRunner> io_;
  ProfileFilterPreferences::SubscriptionFetch fetch_;base::RepeatingCallback<void(FileStatus)> error_;
  const base::RepeatingCallback<bool()> admitted_;
  std::map<GURL,std::shared_ptr<Entry>> entries_;std::atomic<bool> closed_{false};
  base::CallbackListSubscription retirement_;
  base::WeakPtrFactory<Subscriptions> weak_{this};
};
}  // namespace
Manager::Backend ProfileFilterPreferences::SubscriptionBackend(SubscriptionFetch fetch,
    base::RepeatingCallback<GURL(const GURL&)> source_url) {
  CHECK(fetch);
  auto self=base::WrapRefCounted(this);
  auto subscriptions=base::MakeRefCounted<Subscriptions>(storage_,runner_,std::move(fetch),
      base::BindRepeating(&ProfileFilterPreferences::Failed,self),
      base::BindRepeating(&ProfileFilterPreferences::available,self));
  subscriptions->Observe(consumers_closed_.Add(base::BindRepeating([](base::WeakPtr<Subscriptions> subscriptions){
    if(subscriptions)subscriptions->Close();
  },subscriptions->GetWeakPtr())));
  subscription_source_=base::BindRepeating([](base::WeakPtr<Subscriptions> subscriptions,
      const std::string& path,Provider::SourceReply reply) {
    if(subscriptions)subscriptions->ReadView(path,std::move(reply));
    else std::move(reply).Run(Result::kCancelled,{});
  },subscriptions->GetWeakPtr());
  Manager::Backend backend;
  backend.available=base::BindRepeating(&ProfileFilterPreferences::available,self);
  backend.source=base::BindRepeating(&Subscriptions::Source,subscriptions);
  backend.download=base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self,
      scoped_refptr<Subscriptions> subscriptions,const GURL& url,bool from_ui,base::OnceCallback<void(Result)> done){
    if(self->ReadOnly()){std::move(done).Run(Result::kCancelled);return;}
    subscriptions->Download(url,from_ui,std::move(done));
  },self,subscriptions);
  backend.remove=base::BindRepeating(&Subscriptions::Remove,subscriptions);
  backend.cancel=base::BindRepeating(&Subscriptions::Cancel,subscriptions);
  backend.source_url=source_url ? std::move(source_url) : base::BindRepeating(&Subscriptions::SourceUrl,subscriptions);
  backend.observe_close=base::BindRepeating([](scoped_refptr<ProfileFilterPreferences> self,
      scoped_refptr<Subscriptions> subscriptions,base::RepeatingClosure closed){
    return self->consumers_closed_.Add(base::BindRepeating([](base::WeakPtr<Subscriptions> subscriptions,base::RepeatingClosure closed){
      if(subscriptions)subscriptions->Close();closed.Run();
    },subscriptions->GetWeakPtr(),closed));
  },self,subscriptions);
  return backend;
}
void ProfileFilterPreferences::ReadSubscriptionSource(const std::string& path,Provider::SourceReply reply) {
  if(!available() || !subscription_source_) {std::move(reply).Run(Result::kCancelled,{});return;}
  subscription_source_.Run(path,std::move(reply));
}
}  // namespace alloy::site_records
