#include "sound_list.h"

#include <algorithm>

namespace SoundList {

bool isDone(const std::vector<std::string> &done, const std::string &occurrenceId) {
  return std::find(done.begin(), done.end(), occurrenceId) != done.end();
}

void markDone(std::vector<std::string> &done, const std::string &occurrenceId) {
  if (occurrenceId.empty() || isDone(done, occurrenceId)) return;
  done.push_back(occurrenceId);
  if (done.size() > DONE_KEPT) done.erase(done.begin(), done.end() - DONE_KEPT);
}

namespace {

bool listed(const std::vector<Entry> &entries, const std::string &occurrenceId) {
  for (const Entry &entry : entries) {
    if (entry.occurrenceId == occurrenceId) return true;
  }
  return false;
}

} // namespace

std::vector<Entry> merge(const std::vector<Entry> &held, const std::vector<Entry> &incoming,
                         const std::vector<std::string> &done, int64_t now) {
  std::vector<Entry> result;
  for (const Entry &entry : held) {
    if (entry.pushed && !isDone(done, entry.occurrenceId) &&
        now <= entry.epoch + GRACE_SECONDS && !listed(incoming, entry.occurrenceId)) {
      result.push_back(entry);
    }
  }
  for (const Entry &entry : incoming) {
    if (result.size() >= MAX_ENTRIES) break;
    if (!entry.occurrenceId.empty() && !listed(result, entry.occurrenceId)) result.push_back(entry);
  }
  return result;
}

int dueIndex(const std::vector<Entry> &entries, const std::vector<std::string> &done, int64_t now) {
  int best = -1;
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry &entry = entries[i];
    if (now < entry.epoch || now > entry.epoch + GRACE_SECONDS) continue;
    if (isDone(done, entry.occurrenceId)) continue;
    if (best < 0 || entry.epoch < entries[best].epoch) best = static_cast<int>(i);
  }
  return best;
}

int64_t nextEpoch(const std::vector<Entry> &entries, const std::vector<std::string> &done, int64_t now) {
  int64_t next = 0;
  for (const Entry &entry : entries) {
    if (entry.epoch <= now || isDone(done, entry.occurrenceId)) continue;
    if (next == 0 || entry.epoch < next) next = entry.epoch;
  }
  return next;
}

} // namespace SoundList
