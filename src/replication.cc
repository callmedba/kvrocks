#include "replication.h"

#include <signal.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <future>
#include <string>
#include <thread>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <glog/logging.h>

#include "redis_reply.h"
#include "rocksdb_crc32c.h"
#include "util.h"
#include "status.h"
#include "server.h"

FeedSlaveThread::~FeedSlaveThread() {
  delete conn_;
}

Status FeedSlaveThread::Start() {
  try {
    t_ = std::thread([this]() {
      Util::ThreadSetName("feed-slave-thread");
      sigset_t mask, omask;
      sigemptyset(&mask);
      sigemptyset(&omask);
      sigaddset(&mask, SIGCHLD);
      sigaddset(&mask, SIGHUP);
      sigaddset(&mask, SIGPIPE);
      pthread_sigmask(SIG_BLOCK, &mask, &omask);
      // force feed slave thread was scheduled after making the fd blocking,
      // and write "+OK\r\n" response to psync command
      usleep(10000);
      this->loop();
    });
  } catch (const std::system_error &e) {
    conn_ = nullptr;  // prevent connection was freed when failed to start the thread
    return Status(Status::NotOK, e.what());
  }
  return Status::OK();
}

void FeedSlaveThread::Stop() {
  stop_ = true;
  LOG(WARNING) << "Slave thread was terminated, would stop feeding the slave: " << conn_->GetAddr();
}

void FeedSlaveThread::Join() {
  if (t_.joinable()) t_.join();
}

void FeedSlaveThread::checkLivenessIfNeed() {
  if (++interval % 1000) return;
  const auto ping_command = Redis::BulkString("ping");
  auto s = Util::SockSend(conn_->GetFD(), ping_command);
  if (!s.IsOK()) {
    LOG(ERROR) << "Ping slave[" << conn_->GetAddr() << "] err: " << s.Msg()
               << ", would stop the thread";
    Stop();
  }
}

void FeedSlaveThread::loop() {
  uint32_t yield_milliseconds = 2000;
  std::vector<std::string> batch_list;
  while (!IsStopped()) {
    if (!iter_ || !iter_->Valid()) {
      if (iter_) LOG(INFO) << "WAL was rotated, would reopen again";
      if (!srv_->storage_->WALHasNewData(next_repl_seq_)
          || !srv_->storage_->GetWALIter(next_repl_seq_, &iter_).IsOK()) {
        iter_ = nullptr;
        usleep(yield_milliseconds);
        checkLivenessIfNeed();
        continue;
      }
    }
    // iter_ would be always valid here
    auto batch = iter_->GetBatch();
    if (batch.sequence != next_repl_seq_) {
      LOG(ERROR) << "Fatal error encountered, WAL iterator is discrete, some seq might be lost"
                 << ", sequence " << next_repl_seq_ << " expectd, but got " << batch.sequence;
      Stop();
      return;
    }
    auto data = batch.writeBatchPtr->Data();
    batch_list.emplace_back(Redis::BulkString(data));
    // feed the bulks data to slave in batch mode iff the lag was far from the master
    auto latest_seq = srv_->storage_->LatestSeq();
    if (latest_seq - batch.sequence <= 20 || batch_list.size() >= 20) {
      for (const auto &bulk_str : batch_list) {
        auto s = Util::SockSend(conn_->GetFD(), bulk_str);
        if (!s.IsOK()) {
          LOG(ERROR) << "Write error while sending batch to slave: " << s.Msg();
          Stop();
          return;
        }
      }
      batch_list.clear();
    }
    next_repl_seq_ = batch.sequence + batch.writeBatchPtr->Count();
    while (!IsStopped() && !srv_->storage_->WALHasNewData(next_repl_seq_)) {
      usleep(yield_milliseconds);
      checkLivenessIfNeed();
    }
    iter_->Next();
  }
}

void send_string(bufferevent *bev, const std::string &data) {
  auto output = bufferevent_get_output(bev);
  evbuffer_add(output, data.c_str(), data.length());
}

void ReplicationThread::CallbacksStateMachine::ConnEventCB(
    bufferevent *bev, int16_t events, void *state_machine_ptr) {
  if (events & BEV_EVENT_CONNECTED) {
    // call write_cb when connected
    bufferevent_data_cb write_cb;
    bufferevent_getcb(bev, nullptr, &write_cb, nullptr, nullptr);
    if (write_cb) write_cb(bev, state_machine_ptr);
    return;
  }
  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    LOG(ERROR) << "[replication] connection error/eof, reconnect the master";
    // Wait a bit and reconnect
    auto state_m = static_cast<CallbacksStateMachine *>(state_machine_ptr);
    state_m->repl_->repl_state_ = kReplConnecting;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    state_m->Stop();
    state_m->Start();
  }
}

void ReplicationThread::CallbacksStateMachine::SetReadCB(
    bufferevent *bev, bufferevent_data_cb cb, void *state_machine_ptr) {
  bufferevent_enable(bev, EV_READ);
  bufferevent_setcb(bev, cb, nullptr, ConnEventCB, state_machine_ptr);
}

void ReplicationThread::CallbacksStateMachine::SetWriteCB(
    bufferevent *bev, bufferevent_data_cb cb, void *state_machine_ptr) {
  bufferevent_enable(bev, EV_WRITE);
  bufferevent_setcb(bev, nullptr, cb, ConnEventCB, state_machine_ptr);
}

ReplicationThread::CallbacksStateMachine::CallbacksStateMachine(
    ReplicationThread *repl,
    ReplicationThread::CallbacksStateMachine::CallbackList &&handlers)
    : repl_(repl), handlers_(std::move(handlers)) {
  if (!repl_->auth_.empty()) {
    handlers_.emplace_front(CallbacksStateMachine::READ, "auth read", authReadCB);
    handlers_.emplace_front(CallbacksStateMachine::WRITE, "auth write", authWriteCB);
  }
}

void ReplicationThread::CallbacksStateMachine::EvCallback(bufferevent *bev,
                                                          void *ctx) {
  auto self = static_cast<CallbacksStateMachine *>(ctx);
LOOP_LABEL:
  assert(self->handler_idx_ <= self->handlers_.size());
  DLOG(INFO) << "[replication] Execute handler[" << self->getHandlerName(self->handler_idx_) << "]";
  auto st = self->getHandlerFunc(self->handler_idx_)(bev, self->repl_);
  time(&self->repl_->last_io_time_);
  switch (st) {
    case CBState::NEXT:
      ++self->handler_idx_;
      if (self->getHandlerEventType(self->handler_idx_) == WRITE) {
        SetWriteCB(bev, EvCallback, ctx);
      } else {
        SetReadCB(bev, EvCallback, ctx);
      }
      // invoke the read handler (of next step) directly, as the bev might
      // have the data already.
      goto LOOP_LABEL;
    case CBState::AGAIN:
      break;
    case CBState::QUIT:  // state that can not be retry, or all steps are executed.
      bufferevent_free(bev);
      self->bev_ = nullptr;
      self->repl_->repl_state_ = kReplError;
      break;
    case CBState::RESTART:  // state that can be retried some time later
      self->Stop();
      if (self->repl_->stop_flag_) {
        LOG(INFO) << "[replication] Wouldn't restart while the replication thread was stopped";
        break;
      }
      LOG(INFO) << "[replication] Retry in 10 seconds";
      std::this_thread::sleep_for(std::chrono::seconds(10));
      self->Start();
  }
}

void ReplicationThread::CallbacksStateMachine::Start() {
  if (handlers_.empty()) {
    return;
  }
  auto sockaddr_inet = Util::NewSockaddrInet(repl_->host_, repl_->port_);
  auto bev = bufferevent_socket_new(repl_->base_, -1, BEV_OPT_CLOSE_ON_FREE);
  if (bufferevent_socket_connect(bev,
                                 reinterpret_cast<sockaddr *>(&sockaddr_inet),
                                 sizeof(sockaddr_inet)) != 0) {
    // NOTE: Connection error will not appear here, network err will be reported
    // in ConnEventCB. the error here is something fatal.
    LOG(ERROR) << "[replication] Failed to start state machine, err: " << strerror(errno);
  }
  handler_idx_ = 0;
  if (getHandlerEventType(0) == WRITE) {
    SetWriteCB(bev, EvCallback, this);
  } else {
    SetReadCB(bev, EvCallback, this);
  }
  bev_ = bev;
}

void ReplicationThread::CallbacksStateMachine::Stop() {
  if (bev_) {
    bufferevent_free(bev_);
    bev_ = nullptr;
  }
}

ReplicationThread::ReplicationThread(std::string host, uint32_t port,
                                     Server *srv, std::string auth)
    : host_(std::move(host)),
      port_(port),
      auth_(std::move(auth)),
      srv_(srv),
      storage_(srv->storage_),
      repl_state_(kReplConnecting),
      psync_steps_(this,
                   CallbacksStateMachine::CallbackList{
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::WRITE, "dbname write", checkDBNameWriteCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::READ, "dbname read", checkDBNameReadCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::WRITE, "replconf write", replConfWriteCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::READ, "replconf read", replConfReadCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::WRITE, "psync write", tryPSyncWriteCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::READ, "psync read", tryPSyncReadCB
                       },
                       CallbacksStateMachine::CallbackType{
                           CallbacksStateMachine::READ, "batch loop", incrementBatchLoopCB
                       }
                   }),
      fullsync_steps_(this,
                      CallbacksStateMachine::CallbackList{
                          CallbacksStateMachine::CallbackType{
                              CallbacksStateMachine::WRITE, "fullsync write", fullSyncWriteCB
                          },
                          CallbacksStateMachine::CallbackType{
                              CallbacksStateMachine::READ, "fullsync read", fullSyncReadCB}
                      }) {
}

Status ReplicationThread::Start(std::function<void()> &&pre_fullsync_cb,
                                std::function<void()> &&post_fullsync_cb) {
  pre_fullsync_cb_ = std::move(pre_fullsync_cb);
  post_fullsync_cb_ = std::move(post_fullsync_cb);

  // cleanup the old backups, so we can start replication in a clean state
  storage_->PurgeOldBackups(0, 0);

  try {
    t_ = std::thread([this]() {
      Util::ThreadSetName("master-repl");
      this->run();
      assert(stop_flag_);
    });
  } catch (const std::system_error &e) {
    return Status(Status::NotOK, e.what());
  }
  return Status::OK();
}

void ReplicationThread::Stop() {
  if (stop_flag_) return;

  stop_flag_ = true;  // Stopping procedure is asynchronous,
                      // handled by timer
  t_.join();
  LOG(INFO) << "[replication] Stopped";
}

/*
 * Run connect to master, and start the following steps
 * asynchronously
 *  - CheckDBName
 *  - TryPsync
 *  - - if ok, IncrementBatchLoop
 *  - - not, FullSync and restart TryPsync when done
 */
void ReplicationThread::run() {
  base_ = event_base_new();
  if (base_ == nullptr) {
    LOG(ERROR) << "[replication] Failed to create new ev base";
    return;
  }
  psync_steps_.Start();

  auto timer = event_new(base_, -1, EV_PERSIST, EventTimerCB, this);
  timeval tmo{0, 100000};  // 100 ms
  evtimer_add(timer, &tmo);

  event_base_dispatch(base_);
  event_free(timer);
  event_base_free(base_);
}

ReplicationThread::CBState ReplicationThread::authWriteCB(bufferevent *bev,
                                                          void *ctx) {
  auto self = static_cast<ReplicationThread *>(ctx);
  const auto auth_len_str = std::to_string(self->auth_.length());
  send_string(bev, Redis::MultiBulkString({"AUTH", self->auth_}));
  LOG(INFO) << "[replication] Auth request was sent, waiting for response";
  self->repl_state_ = kReplSendAuth;
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::authReadCB(bufferevent *bev,
                                                         void *ctx) {
  char *line;
  size_t line_len;
  auto input = bufferevent_get_input(bev);
  line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
  if (!line) return CBState::AGAIN;
  if (strncmp(line, "+OK", 3) != 0) {
    // Auth failed
    LOG(ERROR) << "[replication] Auth failed: " << line;
    free(line);
    return CBState::RESTART;
  }
  free(line);
  LOG(INFO) << "[replication] Auth response was received, continue...";
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::checkDBNameWriteCB(
    bufferevent *bev, void *ctx) {
  send_string(bev, Redis::MultiBulkString({"_db_name"}));
  auto self = static_cast<ReplicationThread *>(ctx);
  self->repl_state_ = kReplCheckDBName;
  LOG(INFO) << "[replication] Check db name request was sent, waiting for response";
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::checkDBNameReadCB(
    bufferevent *bev, void *ctx) {
  char *line;
  size_t line_len;
  auto input = bufferevent_get_input(bev);
  line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
  if (!line) return CBState::AGAIN;

  if (line[0] == '-') {
    if (isRestoringError(line)) {
      LOG(WARNING) << "The master was restoring the db, retry later";
    } else {
      LOG(ERROR) << "Failed to get the db name, " << line;
    }
    free(line);
    return CBState::RESTART;
  }
  auto self = static_cast<ReplicationThread *>(ctx);
  std::string db_name = self->storage_->GetName();
  if (line_len == db_name.size() && !strncmp(line, db_name.data(), line_len)) {
    // DB name match, we should continue to next step: TryPsync
    free(line);
    LOG(INFO) << "[replication] DB name is valid, continue...";
    return CBState::NEXT;
  }
  LOG(ERROR) << "[replication] Mismatched the db name, local: " << db_name << ", remote: " << line;
  free(line);
  return CBState::RESTART;
}

ReplicationThread::CBState ReplicationThread::replConfWriteCB(
    bufferevent *bev, void *ctx) {
  auto self = static_cast<ReplicationThread *>(ctx);
  send_string(bev,
              Redis::MultiBulkString({"replconf", "listening-port", std::to_string(self->srv_->GetConfig()->port)}));
  self->repl_state_ = kReplReplConf;
  LOG(INFO) << "[replication] replconf request was sent, waiting for response";
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::replConfReadCB(
    bufferevent *bev, void *ctx) {
  char *line;
  size_t line_len;
  auto input = bufferevent_get_input(bev);
  line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
  if (!line) return CBState::AGAIN;

  if (line[0] == '-' && isRestoringError(line)) {
    free(line);
    LOG(WARNING) << "The master was restoring the db, retry later";
    return CBState::RESTART;
  }
  if (strncmp(line, "+OK", 3) != 0) {
    LOG(WARNING) << "[replication] Failed to replconf: " << line+1;
    free(line);
    //  backward compatible with old version that doesn't support replconf cmd
    return CBState::NEXT;
  } else {
    free(line);
    LOG(INFO) << "[replication] replconf is ok, start psync";
    return CBState::NEXT;
  }
}

ReplicationThread::CBState ReplicationThread::tryPSyncWriteCB(
    bufferevent *bev, void *ctx) {
  auto self = static_cast<ReplicationThread *>(ctx);
  auto next_seq = self->storage_->LatestSeq() + 1;
  send_string(bev, Redis::MultiBulkString({"PSYNC", std::to_string(next_seq)}));
  self->repl_state_ = kReplSendPSync;
  LOG(INFO) << "[replication] Try to use psync, next seq: " << next_seq;
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::tryPSyncReadCB(bufferevent *bev,
                                                             void *ctx) {
  char *line;
  size_t line_len;
  auto self = static_cast<ReplicationThread *>(ctx);
  auto input = bufferevent_get_input(bev);
  line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
  if (!line) return CBState::AGAIN;

  if (line[0] == '-' && isRestoringError(line)) {
    free(line);
    LOG(WARNING) << "The master was restoring the db, retry later";
    return CBState::RESTART;
  }
  if (strncmp(line, "+OK", 3) != 0) {
    // PSYNC isn't OK, we should use FullSync
    // Switch to fullsync state machine
    self->fullsync_steps_.Start();
    LOG(INFO) << "[replication] Failed to psync, switch to fullsync";
    free(line);
    return CBState::QUIT;
  } else {
    // PSYNC is OK, use IncrementBatchLoop
    free(line);
    LOG(INFO) << "[replication] PSync is ok, start increment batch loop";
    return CBState::NEXT;
  }
}

ReplicationThread::CBState ReplicationThread::incrementBatchLoopCB(
    bufferevent *bev, void *ctx) {
  char *line = nullptr;
  size_t line_len = 0;
  char *bulk_data = nullptr;
  auto self = static_cast<ReplicationThread *>(ctx);
  self->repl_state_ = kReplConnected;
  auto input = bufferevent_get_input(bev);
  while (true) {
    switch (self->incr_state_) {
      case Incr_batch_size:
        // Read bulk length
        line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
        if (!line) return CBState::AGAIN;
        self->incr_bulk_len_ = line_len > 0 ? std::strtoull(line + 1, nullptr, 10) : 0;
        free(line);
        if (self->incr_bulk_len_ == 0) {
          LOG(ERROR) << "[replication] Invalid increment data size";
          return CBState::RESTART;
        }
        self->incr_state_ = Incr_batch_data;
        break;
      case Incr_batch_data:
        // Read bulk data (batch data)
        if (self->incr_bulk_len_+2 <= evbuffer_get_length(input)) {  // We got enough data
          bulk_data = reinterpret_cast<char *>(evbuffer_pullup(input, self->incr_bulk_len_ + 2));
          std::string bulk_string = std::string(bulk_data, self->incr_bulk_len_);
          // master would send the ping heartbeat packet to check whether the slave was alive or not,
          // don't write ping to db here.
          if (bulk_string != "ping") {
            auto s = self->storage_->WriteBatch(std::string(bulk_data, self->incr_bulk_len_));
            if (!s.IsOK()) {
              LOG(ERROR) << "[replication] CRITICAL - Failed to write batch to local, " << s.Msg();
              return CBState::RESTART;
            }
            self->ParseWriteBatch(bulk_string);
          }
          evbuffer_drain(input, self->incr_bulk_len_ + 2);
          self->incr_state_ = Incr_batch_size;
        } else {
          return CBState::AGAIN;
        }
        break;
    }
  }
}

ReplicationThread::CBState ReplicationThread::fullSyncWriteCB(
    bufferevent *bev, void *ctx) {
  send_string(bev, Redis::MultiBulkString({"_fetch_meta"}));
  auto self = static_cast<ReplicationThread *>(ctx);
  self->repl_state_ = kReplFetchMeta;
  LOG(INFO) << "[replication] Start syncing data with fullsync";
  return CBState::NEXT;
}

ReplicationThread::CBState ReplicationThread::fullSyncReadCB(bufferevent *bev,
                                                             void *ctx) {
  char *line;
  size_t line_len;
  auto self = static_cast<ReplicationThread *>(ctx);
  auto input = bufferevent_get_input(bev);
  switch (self->fullsync_state_) {
    case kFetchMetaID:
      line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
      if (!line) return CBState::AGAIN;
      if (line[0] == '-') {
        LOG(ERROR) << "[replication] Failed to fetch meta id: " << line;
        free(line);
        return CBState::RESTART;
      }
      self->fullsync_meta_id_ = static_cast<rocksdb::BackupID>(
          line_len > 0 ? std::strtoul(line, nullptr, 10) : 0);
      free(line);
      if (self->fullsync_meta_id_ == 0) {
        LOG(ERROR) << "[replication] Invalid meta id received";
        return CBState::RESTART;
      }
      self->storage_->PurgeBackupIfNeed(self->fullsync_meta_id_);
      self->fullsync_state_ = kFetchMetaSize;
      LOG(INFO) << "[replication] Success to fetch meta id: " << self->fullsync_meta_id_;
    case kFetchMetaSize:
      line = evbuffer_readln(input, &line_len, EVBUFFER_EOL_CRLF_STRICT);
      if (!line) return CBState::AGAIN;
      if (line[0] == '-') {
        LOG(ERROR) << "[replication] Failed to fetch meta size: " << line;
        free(line);
        return CBState::RESTART;
      }
      self->fullsync_filesize_ = line_len > 0 ? std::strtoull(line, nullptr, 10) : 0;
      free(line);
      if (self->fullsync_filesize_ == 0) {
        LOG(ERROR) << "[replication] Invalid meta file size received";
        return CBState::RESTART;
      }
      self->fullsync_state_ = kFetchMetaContent;
      LOG(INFO) << "[replication] Success to fetch meta size: " << self->fullsync_filesize_;
    case kFetchMetaContent:
      if (evbuffer_get_length(input) < self->fullsync_filesize_) {
        return CBState::AGAIN;
      }
      auto meta = Engine::Storage::BackupManager::ParseMetaAndSave(
          self->storage_, self->fullsync_meta_id_, input);
      assert(evbuffer_get_length(input) == 0);
      self->fullsync_state_ = kFetchMetaID;

      LOG(INFO) << "[replication] Succeeded fetching meta file, fetching files in parallel";
      self->repl_state_ = kReplFetchSST;
      auto s = self->parallelFetchFile(meta.files);
      if (!s.IsOK()) {
        LOG(ERROR) << "[replication] Failed to parallel fetch files while " + s.Msg();
        return CBState::RESTART;
      }
      LOG(INFO) << "[replication] Succeeded fetching files in parallel, restoring the backup";

      // Restore DB from backup
      self->pre_fullsync_cb_();
      s = self->storage_->RestoreFromBackup();
      if (!s.IsOK()) {
        LOG(ERROR) << "[replication] Failed to restore backup while " + s.Msg() + ", restart fullsync";
        return CBState::RESTART;
      }
      LOG(INFO) << "[replication] Succeeded restoring the backup, fullsync was finish";
      self->post_fullsync_cb_();

      // Switch to psync state machine again
      self->psync_steps_.Start();
      return CBState::QUIT;
  }

  LOG(ERROR) << "Should not arrive here";
  assert(false);
  return CBState::QUIT;
}

Status ReplicationThread::parallelFetchFile(const std::vector<std::pair<std::string, uint32_t>> &files) {
  size_t concurrency = 1;
  if (files.size() > 20) {
    // Use 4 threads to download files in parallel
    concurrency = 4;
  }
  std::atomic<uint32_t> fetch_cnt = {0};
  std::atomic<uint32_t> skip_cnt = {0};
  std::vector<std::future<Status>> results;
  for (size_t tid = 0; tid < concurrency; ++tid) {
    results.push_back(std::async(
        std::launch::async, [this, &files, tid, concurrency, &fetch_cnt, &skip_cnt]() -> Status {
          if (this->stop_flag_) {
            return Status(Status::NotOK, "replication thread was stopped");
          }
          int sock_fd;
          Status s = Util::SockConnect(this->host_, this->port_, &sock_fd);
          if (!s.IsOK()) {
            return Status(Status::NotOK, "connect the server err: " + s.Msg());
          }
          s = this->sendAuth(sock_fd);
          if (!s.IsOK()) {
            close(sock_fd);
            return Status(Status::NotOK, "sned the auth command err: " + s.Msg());
          }
          for (auto f_idx = tid; f_idx < files.size(); f_idx += concurrency) {
            if (this->stop_flag_) {
              return Status(Status::NotOK, "replication thread was stopped");
            }
            const auto &f_name = files[f_idx].first;
            const auto &f_crc = files[f_idx].second;
            // Don't fetch existing files
            if (Engine::Storage::BackupManager::FileExists(this->storage_, f_name)) {
              skip_cnt.fetch_add(1);
              uint32_t cur_skip_cnt = skip_cnt.load();
              uint32_t cur_fetch_cnt = fetch_cnt.load();
              LOG(INFO) << "[skip] "<< f_name << " " << f_crc
                        << ", skip count: " << cur_skip_cnt << ", fetch count: " << cur_fetch_cnt
                        << ", progress: " << cur_skip_cnt+cur_fetch_cnt<< "/" << files.size();
              continue;
            }
            fetch_cnt.fetch_add(1);
            uint32_t cur_skip_cnt = skip_cnt.load();
            uint32_t cur_fetch_cnt = fetch_cnt.load();
            LOG(INFO) << "[fetch] " << f_name << " " << f_crc
                       << ", skip count: " << cur_skip_cnt << ", fetch count: " << cur_fetch_cnt
                       << ", progress: " << cur_skip_cnt+cur_fetch_cnt<< "/" << files.size();
            s = this->fetchFile(sock_fd, f_name, f_crc);
            if (!s.IsOK()) {
              close(sock_fd);
              return Status(Status::NotOK, "fetch file err: " + s.Msg());
            }
          }
          close(sock_fd);
          return Status::OK();
        }));
  }

  // Wait til finish
  for (auto &f : results) {
    Status s = f.get();
    if (!s.IsOK()) return s;
  }
  return Status::OK();
}

Status ReplicationThread::sendAuth(int sock_fd) {
  size_t line_len;

  // Send auth when needed
  if (!auth_.empty()) {
    evbuffer *evbuf = evbuffer_new();
    const auto auth_command = Redis::MultiBulkString({"AUTH", auth_});
    auto s = Util::SockSend(sock_fd, auth_command);
    if (!s.IsOK()) return Status(Status::NotOK, "send auth command err:"+s.Msg());
    while (true) {
      if (evbuffer_read(evbuf, sock_fd, -1) <= 0) {
        evbuffer_free(evbuf);
        return Status(Status::NotOK, std::string("read auth response err: ")+strerror(errno));
      }
      char *line = evbuffer_readln(evbuf, &line_len, EVBUFFER_EOL_CRLF_STRICT);
      if (!line) continue;
      if (strncmp(line, "+OK", 3) != 0) {
        free(line);
        evbuffer_free(evbuf);
        return Status(Status::NotOK, "auth got invalid response");
      }
      free(line);
      break;
    }
    evbuffer_free(evbuf);
  }
  return Status::OK();
}


Status ReplicationThread::fetchFile(int sock_fd, std::string path,
                                    uint32_t crc) {
  size_t line_len, file_size;

  const auto fetch_command = Redis::MultiBulkString({"_fetch_file", path});
  auto s = Util::SockSend(sock_fd, fetch_command);
  if (!s.IsOK()) return Status(Status::NotOK, "send fetch file command err: "+s.Msg());

  evbuffer *evbuf = evbuffer_new();
  // Read file size line
  while (true) {
    if (evbuffer_read(evbuf, sock_fd, -1) <= 0) {
      evbuffer_free(evbuf);
      return Status(Status::NotOK, std::string("read size line err: ")+strerror(errno));
    }
    char *line = evbuffer_readln(evbuf, &line_len, EVBUFFER_EOL_CRLF_STRICT);
    if (!line) continue;
    if (*line == '-') {
      free(line);
      evbuffer_free(evbuf);
      return Status(Status::NotOK, std::string("_fetch_file got err: ")+line);
    }
    file_size = line_len > 0 ? std::strtoull(line, nullptr, 10) : 0;
    free(line);
    break;
  }

  // Write to tmp file
  auto tmp_file = Engine::Storage::BackupManager::NewTmpFile(storage_, path);
  if (!tmp_file) {
    evbuffer_free(evbuf);
    return Status(Status::NotOK, "unable to create tmp file");
  }

  size_t seen_bytes = 0;
  uint32_t tmp_crc = 0;
  char data[1024];
  while (seen_bytes < file_size) {
    if (evbuffer_get_length(evbuf) > 0) {
      auto data_len = evbuffer_remove(evbuf, data, 1024);
      if (data_len == 0) continue;
      if (data_len < 0) {
        evbuffer_free(evbuf);
        return Status(Status::NotOK, "read sst file data error");
      }
      tmp_file->Append(rocksdb::Slice(data, data_len));
      tmp_crc = rocksdb::crc32c::Extend(tmp_crc, data, data_len);
      seen_bytes += data_len;
    } else {
      if (evbuffer_read(evbuf, sock_fd, -1) <= 0) {
        evbuffer_free(evbuf);
        return Status(Status::NotOK, std::string("read sst file data, err: ")+strerror(errno));
      }
    }
  }
  if (crc != tmp_crc) {
    evbuffer_free(evbuf);
    return Status(Status::NotOK, "CRC mismatch");
  }
  evbuffer_free(evbuf);
  // File is OK, rename to formal name
  return Engine::Storage::BackupManager::SwapTmpFile(storage_, path);
}

// Check if stop_flag_ is set, when do, tear down replication
void ReplicationThread::EventTimerCB(int, int16_t, void *ctx) {
  // DLOG(INFO) << "[replication] timer";
  auto self = static_cast<ReplicationThread *>(ctx);
  if (self->stop_flag_) {
    LOG(INFO) << "[replication] Stop ev loop";
    event_base_loopbreak(self->base_);
    self->psync_steps_.Stop();
    self->fullsync_steps_.Stop();
  }
}

rocksdb::Status ReplicationThread::ParseWriteBatch(const std::string &batch_string) {
  rocksdb::WriteBatch write_batch(batch_string);
  WriteBatchHandler write_batch_handler;
  rocksdb::Status status;

  status = write_batch.Iterate(&write_batch_handler);
  if (!status.ok()) return status;
  if (write_batch_handler.IsPublish()) {
    srv_->PublishMessage(write_batch_handler.GetPublishChannel().ToString(),
                         write_batch_handler.GetPublishValue().ToString());
  }

  return rocksdb::Status::OK();
}

bool ReplicationThread::isRestoringError(const char *err) {
  return std::string(err) == "-ERR restoring the db from backup";
}

void WriteBatchHandler::LogData(const rocksdb::Slice &blob) {
  log_data_.Decode(blob);
}

rocksdb::Status WriteBatchHandler::PutCF(uint32_t column_family_id, const rocksdb::Slice &key,
                                         const rocksdb::Slice &value) {
  if (column_family_id == kColumnFamilyIDZSetScore) {
    return rocksdb::Status::OK();
  }

  std::string ns, user_key, sub_key;
  std::vector<std::string> command_args;
  if (column_family_id == kColumnFamilyIDMetadata) {
    ExtractNamespaceKey(key, &ns, &user_key);
    Metadata metadata(kRedisNone);
    metadata.Decode(value.ToString());
    if (metadata.Type() == kRedisString) {
      command_args = {"SET", user_key, value.ToString().substr(5, value.size() - 5)};
      aof_strings_[ns].emplace_back(Command2RESP(command_args));
      if (metadata.expire > 0) {
        command_args = {"EXPIREAT", user_key, std::to_string(metadata.expire)};
        aof_strings_[ns].emplace_back(Command2RESP(command_args));
      }
    } else if (metadata.expire > 0) {
      auto args = log_data_.GetArguments();
      if (args->size() > 0) {
        RedisCommand cmd = static_cast<RedisCommand >(std::stoi((*args)[0]));
        if (cmd == kRedisCmdExpire) {
          command_args = {"EXPIREAT", user_key, std::to_string(metadata.expire)};
          aof_strings_[ns].emplace_back(Command2RESP(command_args));
        }
      }
    }

    return rocksdb::Status::OK();
  }

  if (column_family_id == kColumnFamilyIDDefault) {
    InternalKey ikey(key);
    user_key = ikey.GetKey().ToString();
    sub_key = ikey.GetSubKey().ToString();
    ns = ikey.GetNamespace().ToString();
    switch (log_data_.GetRedisType()) {
      case kRedisHash:command_args = {"HSET", user_key, sub_key, value.ToString()};
        break;
      case kRedisList: {
        auto args = log_data_.GetArguments();
        if (args->size() < 1) {
          LOG(ERROR) << "Fail to parse write_batch in putcf type list : args error ,should at least contain cmd";
          return rocksdb::Status::OK();
        }
        RedisCommand cmd = static_cast<RedisCommand >(std::stoi((*args)[0]));
        switch (cmd) {
          case kRedisCmdLSet:
            if (args->size() < 2) {
              LOG(ERROR) << "Fail to parse write_batch in putcf cmd lset : args error ,should contain lset index";
              return rocksdb::Status::OK();
            }
            command_args = {"LSET", user_key, (*args)[1], value.ToString()};
            break;
          case kRedisCmdLInsert:
            if (firstSeen_) {
              if (args->size() < 4) {
                LOG(ERROR)
                    << "Fail to parse write_batch in putcf cmd linsert : args error ,should contain before pivot value ";
                return rocksdb::Status::OK();
              }
              command_args = {"LINSERT", user_key, (*args)[1] == "1" ? "before" : "after", (*args)[2], (*args)[3]};
              firstSeen_ = false;
            }
            break;
          default:command_args = {cmd == kRedisCmdLPush ? "LPUSH" : "RPUSH", user_key, value.ToString()};
        }
        break;
      }
      case kRedisSet:command_args = {"SADD", user_key, sub_key};
        break;
      case kRedisZSet: {
        double score = DecodeDouble(value.data());
        command_args = {"ZADD", user_key, std::to_string(score), sub_key};
        break;
      }
      default: break;
    }
  }

  if (!command_args.empty()) {
    aof_strings_[ns].emplace_back(Command2RESP(command_args));
  }
  return rocksdb::Status::OK();
}

std::string WriteBatchHandler::Command2RESP(const std::vector<std::string> &cmd_args) {
  std::string output;
  output.append("*" + std::to_string(cmd_args.size()) + CRLF);
  for (const auto &arg : cmd_args) {
    output.append("$" + std::to_string(arg.size()) + CRLF);
    output.append(arg + CRLF);
  }
  return output;
}
