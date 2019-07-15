// -*- mode: c++; indent-tabs-mode: nil; tab-width: 2 -*-
#pragma once

// @TODO: - priority handling of translation requests (for faster premium service)


#include "data/batch_generator.h"
#include "data/corpus.h"
#include "data/shortlist.h"
#include "data/text_input.h"

#include "3rd_party/threadpool.h"
#include "translator/history.h"
#include "translator/output_collector.h"
#include "translator/output_printer.h"

#include "models/model_task.h"
#include "translator/scorers.h"

#include "translation_worker.h"
#include "common/logging.h"
#include "queue.h"
#include "queued_input.h"
#include <map>

#include <string>
#include "translation_worker.h"
#include "translation_job.h"
#include <functional>
#include <mutex>
#include <thread>

extern Logger logger;

namespace marian {
namespace server {
// This should actually go into vocab.*
// Also it should be merged with the loadOrCreate code in corpus_base.cpp
// and refactored as a separate function (that then goes into vocab.*).
std::vector<Ptr<Vocab const> >
loadVocabularies(Ptr<Options> options) {
  // @TODO: parallelize vocab loading for faster startup
  auto vfiles = options->get<std::vector<std::string>>("vocabs");
  // with the current setup, we need at least two vocabs: src and trg
  ABORT_IF(vfiles.size() < 2, "Insufficient number of vocabularies.");
  std::vector<Ptr<Vocab const> > vocabs(vfiles.size());
  std::unordered_map<std::string,Ptr<Vocab>> vmap;
  for (size_t i = 0; i < vocabs.size(); ++i) {
    auto m = vmap.emplace(std::make_pair(vfiles[i],Ptr<Vocab>()));
    if (m.second) { // new: load the vocab
      m.first->second = New<Vocab>(options, i);
      m.first->second->load(vfiles[i]);
    }
    vocabs[i] = m.first->second;
  }
  return vocabs;
}

template<class Search>
class TranslationService {
public:
  typedef std::function<void (uint64_t ejid, Ptr<History const> h)>
  ResponseHandler;

private:
  // Note to callback n00bs: see this:
  // https://oopscenities.net/2012/02/24/c11-stdfunction-and-stdbind/

  typedef TranslationWorker<Search>
  Worker;

  // bits and pieces for translating
  Ptr<Options> options_;
  std::vector<Ptr<Vocab const>> vocabs_;
  std::vector<Ptr<Worker>> workers_;
  Ptr<data::QueuedInput> jq_;
  Ptr<data::ShortlistGenerator const> slgen_;

  // bits and pieces for callbacks
  std::mutex lock_; // for management of pending callbacks
  typedef std::pair<Ptr<Job>, std::promise<Ptr<Job const>>> JobEntry;
  std::unordered_map<uint64_t, JobEntry> scheduled_jobs_;

  bool keep_going_{true};
  std::atomic_ullong job_ctr_{0};

  void callback_(Ptr<History const> h) {
    // This function is called by the workers once translations are available.

    JobEntry entry;
    { // remove the job / promise pair from the pool of scheduled jobs
      std::lock_guard<std::mutex> lock(lock_);
      auto m = scheduled_jobs_.find(h->GetLineNum());
      if (m == scheduled_jobs_.end()) return; // job was cancelled (not yet implemented)
      entry = std::move(m->second);
      scheduled_jobs_.erase(m);
    }

    // extract translations from history and fulfil the promise
    entry.first->finish(h, isRight2LeftDecoder(), *vocabs_.back());
    entry.second.set_value(entry.first);
  }

public:
  TranslationService(Ptr<Options> options)
    : options_(options) {
  }

  ~TranslationService() {
    stop();
  }

  void stop() {
    for (auto& w: workers_) w->stop();
    for (auto& w: workers_) w->join();
  }

  void start() {
    keep_going_ = true;
    vocabs_ = loadVocabularies(options_);

    if(options_->hasAndNotEmpty("shortlist")) {
      Ptr<data::ShortlistGenerator const> slgen;
      slgen_ = New<data::LexicalShortlistGenerator>                   \
        (options_, vocabs_.front(), vocabs_.back(),
         /*srcIdx=*/ 0, /*trgIdx=*/ 1,
         /*shared (vocab) = */ vocabs_.front() == vocabs_.back());
    }

    jq_.reset(new data::QueuedInput(vocabs_,options_));
    auto devices = Config::getDevices(options_);
    for (auto d: devices) {
      // wrap callback in a lambda function because it's a non static
      // member function:
      auto cb = [=](Ptr<History const> h) { this->callback_(h); };
      workers_.push_back(New<Worker>(d, vocabs_, slgen_, jq_, cb, options_));
      workers_.back()->start();
    }
  }

  std::pair<uint64_t, std::future<Ptr<Job const>>>
  push(uint64_t ejid, std::string const& input, size_t const nbest=1,
       size_t const priority=0) {
    auto job = New<Job>(++job_ctr_, ejid, input, nbest, priority);
    if (!jq_->push(job)) {
      job->error = New<Error>("Could not push to Queue.");
      std::promise<Ptr<Job const>> prom;
      prom.set_value(job);
      return std::make_pair(job->internal_id,prom.get_future());
    }
    JobEntry* entry;
    {
      std::lock_guard<std::mutex> lock(lock_);
      entry = &scheduled_jobs_[job->internal_id];
    }
    entry->first = job;
    LOG(info, "Pushed job No {}; {} jobs queued up.", job->internal_id, jq_->size());
    return std::make_pair(job->internal_id, entry->second.get_future());
  }

  Ptr<Vocab const> vocab(int i) const {
    if (i < 0) i += vocabs_.size();
    return vocabs_.at(i);
  }

  bool isRight2LeftDecoder() const {
    return options_->get<bool>("right-left");
  }

  std::string
  translate(std::string const& srcText) {
    // @TODO: add priority for QoS differentiation [UG]
    std::vector<std::future<Ptr<Job const>>> ftrans;
    std::istringstream buf(srcText);
    std::string line;
    for (size_t linectr = 0; getline(buf,line); ++linectr) {
      ftrans.push_back(push(linectr,line).second);
    }
    std::ostringstream obuf;
    for (auto& t: ftrans) {
      Ptr<Job const> j = t.get();
      obuf << j->translation << std::endl;
    }
    std::string translation = obuf.str();
    if (srcText.size() && srcText.back() != '\n')
      translation.pop_back();
    return translation;
  }
};





}} // end of namespace marian::server
