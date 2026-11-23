// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/linux_lock/protection.h"
#include "brave/alloy/guest/linux_lock/child_launch.h"
#include "brave/alloy/guest/linux_lock/test_private_scratch.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace alloy::guest {
namespace {
extern "C" char** environ;

LinuxSandboxFacts ObservedSandbox() {
  LinuxSandboxFacts facts;
  facts.observed = true;
  facts.browser_nnp = true;
  facts.renderer_nnp = true;
  facts.renderer_seccomp = 2;
  facts.route = "userns";
  return facts;
}

TEST(LinuxProtectionReportTest, RequiresObservedEnforcementAndSandbox) {
  auto facts = ObservedSandbox();
  EXPECT_FALSE(LinuxProtectionFields(2, facts));
  facts.observed = false;
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
  facts = ObservedSandbox();
  facts.renderer_seccomp = 0;
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
  facts = ObservedSandbox();
  facts.renderer_capabilities = 1;
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
  facts = ObservedSandbox();
  facts.browser_nnp = false;
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
  facts = ObservedSandbox();
  facts.route.clear();
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
  facts = ObservedSandbox();
  facts.route = "setuid";
  EXPECT_FALSE(LinuxProtectionFields(4, facts));
}

TEST(LinuxProtectionReportTest, SeparatesBoundaryLimitsAndUsernsRequirement) {
  auto fields = LinuxProtectionFields(4, ObservedSandbox());
  ASSERT_TRUE(fields);
  EXPECT_EQ((*fields)[0], "data-writes-denied");
  EXPECT_EQ((*fields)[2], "verified-tmpfs");
  EXPECT_NE((*fields)[3].find("never-used-in-guest-mode"), std::string::npos);
  EXPECT_NE((*fields)[4].find("not-isolated-from-other-same-user-processes"), std::string::npos);
  EXPECT_NE((*fields)[1].find("private-PID-namespace-procfs; separate-POSIX-session"), std::string::npos);
  EXPECT_NE((*fields)[4].find("non-process-procfs-controls-not-virtualized"), std::string::npos);
  EXPECT_EQ((*fields)[4].find("private-process-view-planned-after-first-release"), std::string::npos);
  EXPECT_NE((*fields)[4].find("metadata_and_atime"), std::string::npos);
  EXPECT_NE((*fields)[4].find("RAM-may-swap"), std::string::npos);
  EXPECT_NE((*fields)[4].find("not-OS-enforced"), std::string::npos);
  EXPECT_NE((*fields)[4].find("observer-handle-retains-memory-after-exit"), std::string::npos);
  EXPECT_EQ((*fields)[4].find("named-RAM-may-remain"), std::string::npos);
}

TEST(LinuxLockLaunchTest, RemappingCannotClobberALaterSource) {
  int registration[2], remote[2];
  ASSERT_EQ(pipe2(registration, O_CLOEXEC), 0);
  ASSERT_EQ(pipe2(remote, O_CLOEXEC), 0);
  ASSERT_EQ(write(registration[1], "REGISTER", 8), 8);
  ASSERT_EQ(write(remote[1], "REMOTE", 6), 6);
  close(registration[1]);
  close(remote[1]);
  const alloy_lock_fd_map maps[] = {{remote[0], registration[0]}, {registration[0], 196}};
  int copies[2];
  posix_spawn_file_actions_t actions;
  ASSERT_TRUE(alloy_lock_spawn_actions(&actions, maps, 2, copies));
  char program[] = "import os; raise SystemExit(os.read(196,64) != b'REGISTER')";
  char executable[] = "/usr/bin/python3", option[] = "-c";
  char* args[] = {executable, option, program, nullptr};
  pid_t pid = -1;
  int launched = posix_spawn(&pid, executable, &actions, nullptr, args, environ);
  alloy_lock_spawn_actions_destroy(&actions, 2, copies);
  close(registration[0]);
  close(remote[0]);
  ASSERT_EQ(launched, 0);
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

class LinuxLockTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(disk_.CreateUniqueTempDir());
    sentinel_ = disk_.GetPath().AppendASCII("sentinel").value();
    ASSERT_TRUE(base::WriteFile(base::FilePath(sentinel_), "durable-sentinel"));
  }

  // Restrictions and the scratch mount belong only to a fixture subprocess.
  // No outside directory or mount handle is retained; kernel exit releases it.
  int Run(const std::function<bool()>& body,
          const std::function<bool()>& prepare = [] { return true; }) {
    int pipe_fds[2];
    EXPECT_EQ(pipe(pipe_fds), 0);
    pid_t pid = fork();
    EXPECT_GE(pid, 0);
    if (!pid) {
      close(pipe_fds[0]);
      if (!alloy_test_private_scratch() || !alloy_lock_prepare_scratch() || !alloy_lock_writer_paths()) _exit(10);
      char alias[64], scratch[PATH_MAX];
      snprintf(alias, sizeof(alias), "/proc/self/fd/%d", alloy_lock_scratch_fd());
      ssize_t count = readlink(alias, scratch, sizeof(scratch));
      if (count <= 0 || write(pipe_fds[1], scratch, count) != count) _exit(11);
      close(pipe_fds[1]);
      if (!prepare() || !alloy_lock_enforce()) _exit(12);
      _exit(body() ? 0 : 13);
    }
    close(pipe_fds[1]);
    char scratch[PATH_MAX];
    ssize_t count = read(pipe_fds[0], scratch, sizeof(scratch));
    close(pipe_fds[0]);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    if (count > 0) {
      EXPECT_EQ(std::string(scratch, count), ALLOY_GUEST_SCRATCH_PATH);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
  bool Unchanged() {
    std::string value;
    return base::ReadFileToString(base::FilePath(sentinel_), &value) && value == "durable-sentinel";
  }
  base::ScopedTempDir disk_;
  std::string sentinel_;
};

TEST_F(LinuxLockTest, VerifiedRamWritesAndNativePaths) {
  EXPECT_EQ(Run([] {
    for (const char* name : {"DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS"}) {
      const char* value = getenv(name);
      if (!value || std::string(value) != "disabled:") return false;
    }
    for (const char* name : {"HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
         "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "ALLOY_GUEST_CEF_ROOT"}) {
      const char* value = getenv(name);
      if (!value || !std::string(value).starts_with("/dev/shm/")) return false;
      int dir = open(value, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir < 0) return false;
      int fd = openat(dir, "native-writer", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      close(dir);
      if (fd < 0) return false;
      bool ok = write(fd, "ram", 3) == 3;
      close(fd);
      if (!ok) return false;
    }
    return alloy_lock_scratch_valid() != 0;
  }), 0);
}

TEST_F(LinuxLockTest, KernelOwnsUnreadableScratchWithoutFollowingSymlinks) {
  struct stat before;
  ASSERT_EQ(stat(disk_.GetPath().value().c_str(), &before), 0);
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (!pid) {
    if (!alloy_test_private_scratch() || !alloy_lock_prepare_scratch()) _exit(10);
    int root = alloy_lock_scratch_fd();
    char alias[64], path[PATH_MAX];
    snprintf(alias, sizeof(alias), "/proc/self/fd/%d", root);
    ssize_t count = readlink(alias, path, sizeof(path) - 1);
    if (count <= 0) _exit(11);
    path[count] = 0;
    if (mkdirat(root, "blocked", 0700)) _exit(12);
    int child = openat(root, "blocked", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (child < 0 || mkdirat(child, "nested", 0700)) _exit(12);
    int nested = openat(child, "nested", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (nested < 0) _exit(12);
    int data = openat(nested, "data", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (data < 0) _exit(12);
    close(data);
    if (symlinkat(disk_.GetPath().value().c_str(), child, "outside") ||
        symlinkat(disk_.GetPath().value().c_str(), root, "outside") ||
        fchmod(nested, 0000) || fchmod(child, 0000)) _exit(12);
    alloy_lock_remove_scratch();
    // Closing a setup pin does not walk or chmod files. Mount reclamation is
    // the kernel's responsibility when this last namespace member exits.
    struct stat blocked;
    bool private_tree = !fstat(child, &blocked) && (blocked.st_mode & 0777) == 0 &&
                        alloy_lock_scratch_fd() < 0;
    close(nested);
    close(child);
    _exit(private_tree ? 0 : 13);
  }
  int status;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  struct stat after;
  ASSERT_EQ(stat(disk_.GetPath().value().c_str(), &after), 0);
  EXPECT_EQ(after.st_mode, before.st_mode);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, DeniesDurableWritesAndBothTruncateForms) {
  EXPECT_EQ(Run([this] {
    int fd = open(sentinel_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd >= 0 || errno != EACCES) return false;
    if (!truncate(sentinel_.c_str(), 0) || errno != EACCES) return false;
    fd = open(sentinel_.c_str(), O_RDONLY | O_TRUNC | O_CLOEXEC);
    return fd < 0 && errno == EACCES;
  }), 0);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, PolicyPinsRamWithoutAnEngineDirectoryCapability) {
  EXPECT_EQ(Run([this] {
    std::string ram_file = std::string(getenv("TMPDIR")) + "/after-setup-close";
    if (!alloy_lock_drop_setup_capability() || alloy_lock_scratch_fd() >= 0) return false;
    int ram = open(ram_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (ram < 0) return false;
    bool wrote = write(ram, "ram", 3) == 3;
    close(ram);
    int disk = open(sentinel_.c_str(), O_WRONLY | O_CLOEXEC);
    return wrote && disk < 0 && errno == EACCES;
  }), 0);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, DeniesCreationRemovalRenameAndHardLink) {
  const std::string outside = disk_.GetPath().AppendASCII("new").value();
  EXPECT_EQ(Run([this, &outside] {
    if (!mkdir(outside.c_str(), 0700) || errno != EACCES) return false;
    if (!unlink(sentinel_.c_str()) || errno != EACCES) return false;
    if (!rename(sentinel_.c_str(), outside.c_str()) || errno != EACCES) return false;
    if (!link(sentinel_.c_str(), outside.c_str()) || errno != EACCES) return false;
    return true;
  }), 0);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, SymlinkAndProcMagicLinkDoNotGrantDiskTargets) {
  int original = open(sentinel_.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(original, 0);
  EXPECT_EQ(Run([original] {
    int fd = openat(alloy_lock_scratch_fd(), "outside-alias", O_WRONLY | O_CLOEXEC);
    if (fd >= 0 || errno != EACCES) return false;
    char alias[64];
    snprintf(alias, sizeof(alias), "/proc/self/fd/%d", original);
    fd = open(alias, O_WRONLY | O_CLOEXEC);
    return fd < 0 && errno == EACCES;
  }, [this] { return !symlinkat(sentinel_.c_str(), alloy_lock_scratch_fd(), "outside-alias"); }), 0);
  close(original);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, RefusesNewBindAndVolumeAliasesBeneathRam) {
  EXPECT_EQ(Run([this] {
    const std::string target = std::string(getenv("TMPDIR")) + "/mount-alias";
    if (mkdir(target.c_str(), 0700)) return false;
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS))
      return errno == EPERM || errno == EACCES;
    auto map = [](const char* path, const std::string& text) {
      int fd = open(path, O_WRONLY | O_CLOEXEC);
      if (fd < 0) return false;
      bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
      close(fd);
      return ok;
    };
    if (!map("/proc/self/setgroups", "deny") ||
        !map("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1") ||
        !map("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1"))
      return false;
    // The restricted process cannot turn an allowed RAM pathname into a new
    // durable bind/volume grant, even with new namespace capabilities and
    // before its later Chromium seccomp filter.
    errno = 0;
    if (!mount(disk_.GetPath().value().c_str(), target.c_str(), nullptr, MS_BIND, nullptr) ||
        (errno != EPERM && errno != EACCES)) return false;
    errno = 0;
    return mount("tmpfs", target.c_str(), "tmpfs", 0, "size=1m") &&
        (errno == EPERM || errno == EACCES);
  }), 0);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, ForkAndExecInheritWriteRefusal) {
  EXPECT_EQ(Run([this] {
    pid_t child = fork();
    if (!child) {
      execl("/bin/sh", "sh", "-c", "exec 2>/dev/null; : > \"$1\"", "sh", sentinel_.c_str(), nullptr);
      _exit(127);
    }
    int status;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 2;
  }), 0);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, DocumentsMetadataGapWithoutAdvertisingReadOnlyView) {
  EXPECT_EQ(Run([this] {
    struct timespec times[2] = {{123456789, 0}, {123456789, 0}};
    return !chmod(sentinel_.c_str(), 0640) && !utimensat(AT_FDCWD, sentinel_.c_str(), times, 0);
  }), 0);
  struct stat st;
  ASSERT_EQ(stat(sentinel_.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777u, 0640u);
  EXPECT_EQ(st.st_mtime, 123456789);
  EXPECT_TRUE(Unchanged());
}

TEST_F(LinuxLockTest, DocumentsPreopenedWriteFdAndMappingGaps) {
  int fd = open(sentinel_.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  void* mapping = mmap(nullptr, 16, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ASSERT_NE(mapping, MAP_FAILED);
  EXPECT_EQ(Run([fd, mapping] {
    static_cast<char*>(mapping)[0] = 'M';
    return pwrite(fd, "F", 1, 1) == 1 && !msync(mapping, 16, MS_SYNC);
  }), 0);
  munmap(mapping, 16);
  close(fd);
  std::string value;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(sentinel_), &value));
  EXPECT_EQ(value.substr(0, 2), "MF");
}

TEST_F(LinuxLockTest, NativeTruncateRefusedButSameUserProcControlGapRemains) {
  // Only this fixture's disposable sibling is a target. This is an explicit
  // limit receipt, not a claim that the current policy isolates processes.
  pid_t target = fork();
  ASSERT_GE(target, 0);
  if (!target) { for (;;) pause(); }
  const std::string path = "/proc/" + std::to_string(target) + "/oom_score_adj";
  int result = Run([&path] {
    int native = creat(path.c_str(), 0666);
    if (native >= 0 || errno != EACCES) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool changed = write(fd, "321", 3) == 3;
    close(fd);
    return changed;
  });
  std::string observed;
  bool read = base::ReadFileToString(base::FilePath(path), &observed);
  EXPECT_EQ(kill(target, SIGKILL), 0);
  EXPECT_EQ(waitpid(target, nullptr, 0), target);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(read);
  EXPECT_EQ(observed, "321\n");
}
}  // namespace
}  // namespace alloy::guest
