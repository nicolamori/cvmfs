/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_CATALOG_TRAVERSAL_H_
#define CVMFS_CATALOG_TRAVERSAL_H_

#include <cassert>
#include <limits>
#include <set>
#include <stack>
#include <string>
#include <vector>

#include "catalog.h"
#include "compression.h"
#include "crypto/signature.h"
#include "history_sqlite.h"
#include "manifest.h"
#include "object_fetcher.h"
#include "util/concurrency.h"
#include "util/logging.h"

namespace catalog {
class Catalog;
class WritableCatalog;
}

namespace swissknife {

/**
 * Callback data which has to be implemented by the registered callback
 * functions/methods (see Observable<> for further details)
 * @param catalog             the catalog object which needs to be processed
 * @param catalog_hash        the content hash of the catalog
 * @param tree_level          the depth in the nested catalog tree
 *                            (starting at zero)
 * @param file_size           the size of the downloaded catalog database file
 * @param history_depth       the distance from the current HEAD revision
 *                            (current HEAD has history_depth 0)
 */
template <class CatalogT>
struct CatalogTraversalData {
  CatalogTraversalData(const CatalogT     *catalog,
                       const shash::Any   &catalog_hash,
                       const unsigned      tree_level,
                       const size_t        file_size,
                       const unsigned int  history_depth)
  : catalog(catalog)
  , catalog_hash(catalog_hash)
  , tree_level(tree_level)
  , file_size(file_size)
  , history_depth(history_depth) {}

  const CatalogT     *catalog;
  const shash::Any    catalog_hash;
  const unsigned int  tree_level;
  const size_t        file_size;
  const unsigned int  history_depth;
};


/**
 * A layer to extract information from a catalog.  Users of the catalog
 * traversal can provide derived classes to overwrite behavior.  Currently used
 * to get the last-modified timestamp in configurable manner: for the garbage
 * collection, the timestamp of the catalog hash in the reflog counts, which
 * is the same or newer than the one stored in the catalog.
 */
template <class CatalogT>
class CatalogTraversalInfoShim {
 public:
  virtual ~CatalogTraversalInfoShim() { }

  // Default implementation: use the catalog's timestamp
  virtual uint64_t GetLastModified(const CatalogT *catalog) {
    return catalog->GetLastModified();
  }
};


/**
 * A base class for CatalogTraversal and CatalogTraversalParallel implementing
 * common functionality. Actual traversal classes inherit from this class.
 *
 */
template <class ObjectFetcherT>
class CatalogTraversalBase
  : public Observable<CatalogTraversalData<typename ObjectFetcherT::CatalogTN> >
{
 public:
  typedef ObjectFetcherT                      ObjectFetcherTN;
  typedef typename ObjectFetcherT::CatalogTN  CatalogTN;
  typedef typename ObjectFetcherT::HistoryTN  HistoryTN;
  typedef CatalogTraversalData<CatalogTN>     CallbackDataTN;

  /**
   * @param repo_url             the path to the repository to be traversed:
   *                             -> either absolute path to the local catalogs
   *                             -> or an URL to a remote repository
   * @param repo_name            fully qualified repository name (used for remote
   *                             repository signature check) (optional)
   * @param repo_keys            a comma separated list of public key file
   *                             locations to verify the repository manifest file
   * @param history              depth of the desired catalog history traversal
   *                             (default: 0 - only HEAD catalogs are traversed)
   * @param timestamp            timestamp of history traversal threshold
   *                             (default: 0 - no threshold, traverse everything)
   * @param no_repeat_history    keep track of visited catalogs and don't re-visit
   *                             them in previous revisions
   * @param no_close             do not close catalogs after they were attached
   *                             (catalogs retain their parent/child pointers)
   * @param ignore_load_failure  suppressed an error message if a catalog file
   *                             could not be loaded (i.e. was sweeped before by
   *                             a garbage collection run)
   * @param quiet                silence messages that would go to stderr
   * @param tmp_dir              path to the temporary directory to be used
   *                             (default: /tmp)
   *
   * ONLY FOR CatalogTraversalParallel
   * @param num_threads           number of threads concurrently traversing
   *                              the catalog trees; hint: set up based on
   *                              CPU utilization and optimal number of
   *                              download threads
   *                              (default: 8)
   *
   * @param serialize_callbacks   do not call multiple catalog callbacks
   *                              concurrently; ensures safe access to shared
   *                              variables inside the callback; turn off if
   *                              the registered callback is thread-safe
   *                              (default: true)
   */
  struct Parameters {
    Parameters()
      : object_fetcher(NULL)
      , history(kNoHistory)
      , timestamp(kNoTimestampThreshold)
      , no_repeat_history(false)
      , no_close(false)
      , ignore_load_failure(false)
      , quiet(false)
      , num_threads(8)
      , serialize_callbacks(true) {}

    static const unsigned int kFullHistory;
    static const unsigned int kNoHistory;
    static const time_t       kNoTimestampThreshold;

    ObjectFetcherT *object_fetcher;

    unsigned int    history;
    time_t          timestamp;
    bool            no_repeat_history;
    bool            no_close;
    bool            ignore_load_failure;
    bool            quiet;
    // These parameters only apply for CatalogTraversalParallel
    unsigned int    num_threads;
    bool            serialize_callbacks;
  };

  enum TraversalType {
    kBreadthFirst,
    kDepthFirst
  };

  explicit CatalogTraversalBase(const Parameters &params)
    : object_fetcher_(params.object_fetcher)
    , catalog_info_shim_(&catalog_info_default_shim_)
    , default_history_depth_(params.history)
    , default_timestamp_threshold_(params.timestamp)
    , no_close_(params.no_close)
    , ignore_load_failure_(params.ignore_load_failure)
    , no_repeat_history_(params.no_repeat_history)
    , error_sink_((params.quiet) ? kLogDebug : kLogStderr)
  {
    assert(object_fetcher_ != NULL);
  }

  /**
   * Starts the traversal process.
   * After calling this methods CatalogTraversal will go through all catalogs
   * and call the registered callback methods for each found catalog.
   * If something goes wrong in the process, the traversal will be cancelled.
   *
   * @param type   breadths or depth first traversal
   * @return       true, when all catalogs were successfully processed. On
   *               failure the traversal is cancelled and false is returned.
   */
  virtual bool Traverse(const TraversalType type = kBreadthFirst) = 0;

  /**
   * Starts the traversal process at the catalog pointed to by the given hash
   *
   * @param root_catalog_hash  the entry point into the catalog traversal
   * @param type               breadths or depth first traversal
   * @return                   true when catalogs were successfully traversed
   */
  virtual bool Traverse(const shash::Any &root_catalog_hash,
                        const TraversalType type = kBreadthFirst) = 0;

  /**
   * Traverse a list of revisions represented by root catalogs from first
   * to last. DO NOT traverse previous revisions based on history and
   * timestamp threshold settings.
   *
   * @param catalog_list  list of root catalog hashes
   * @param type          breadth- or depth- first traversal
   * @return              true on success
   */
  virtual bool TraverseList(const std::vector<shash::Any> &catalog_list,
    const TraversalType type = kBreadthFirst) = 0;

  /**
   * Starts the traversal process at the catalog pointed to by the given hash
   * but doesn't traverse into predecessor catalog revisions. This overrides the
   * TraversalParameter settings provided at construction.
   *
   * @param root_catalog_hash  the entry point into the catalog traversal
   * @param type               breadths or depth first traversal
   * @return                   true when catalogs were successfully traversed
   */
  virtual bool TraverseRevision(const shash::Any     &root_catalog_hash,
                        const TraversalType type = kBreadthFirst) = 0;

  /**
   * Figures out all named tags in a repository and uses all of them as entry
   * points into the traversal process.
   *
   * @param type  breadths or depth first traversal
   * @return      true when catalog traversal successfully finished
   */
  virtual bool TraverseNamedSnapshots(
    const TraversalType type = kBreadthFirst)
  {
    typedef std::vector<shash::Any> HashList;

    UniquePtr<HistoryTN> tag_db;
    const typename ObjectFetcherT::Failures retval =
                                         object_fetcher_->FetchHistory(&tag_db);
    switch (retval) {
      case ObjectFetcherT::kFailOk:
        break;

      case ObjectFetcherT::kFailNotFound:
        LogCvmfs(kLogCatalogTraversal, kLogDebug,
                 "didn't find a history database to traverse");
        return true;

      default:
        LogCvmfs(kLogCatalogTraversal, kLogStderr,
                 "failed to download history database (%d - %s)",
                 retval, Code2Ascii(retval));
        return false;
    }

    HashList root_hashes;
    bool success = tag_db->GetHashes(&root_hashes);
    assert(success);
    return TraverseList(root_hashes, type);
  }

  void SetCatalogInfoShim(CatalogTraversalInfoShim<CatalogTN> *shim) {
    catalog_info_shim_ = shim;
  }

 protected:
  typedef std::set<shash::Any> HashSet;

   /**
   * This struct keeps information about a catalog that still needs to be
   * traversed by a currently running catalog traversal process.
   */
  struct CatalogJob {
    CatalogJob(const std::string  &path,
               const shash::Any   &hash,
               const unsigned      tree_level,
               const unsigned      history_depth,
                     CatalogTN    *parent = NULL) :
      path(path),
      hash(hash),
      tree_level(tree_level),
      history_depth(history_depth),
      parent(parent),
      catalog_file_size(0),
      ignore(false),
      catalog(NULL),
      referenced_catalogs(0),
      postponed(false) {}

    bool IsRootCatalog() const { return tree_level == 0; }

    CallbackDataTN GetCallbackData() const {
      return CallbackDataTN(catalog, hash, tree_level,
                            catalog_file_size, history_depth);
    }

    // initial state description
    const std::string   path;
    const shash::Any    hash;
    const unsigned      tree_level;
    const unsigned      history_depth;
          CatalogTN    *parent;

    // dynamic processing state (used internally)
    std::string   catalog_file_path;
    size_t        catalog_file_size;
    bool          ignore;
    CatalogTN    *catalog;
    unsigned int  referenced_catalogs;
    bool          postponed;
  };

  bool PrepareCatalog(CatalogJob *job) {
    const typename ObjectFetcherT::Failures retval =
      object_fetcher_->FetchCatalog(job->hash,
                                    job->path,
                                    &job->catalog,
                                    !job->IsRootCatalog(),
                                    job->parent);
    switch (retval) {
      case ObjectFetcherT::kFailOk:
        break;

      case ObjectFetcherT::kFailNotFound:
        if (ignore_load_failure_) {
          LogCvmfs(kLogCatalogTraversal, kLogDebug, "ignoring missing catalog "
                                                    "%s (swept before?)",
                   job->hash.ToString().c_str());
          job->ignore = true;
          return true;
        }

      default:
        LogCvmfs(kLogCatalogTraversal, error_sink_, "failed to load catalog %s "
                                                    "(%d - %s)",
                 job->hash.ToStringWithSuffix().c_str(),
                 retval, Code2Ascii(retval));
        return false;
    }

    // catalogs returned by ObjectFetcher<> are managing their database files by
    // default... we need to manage this file manually here
    job->catalog->DropDatabaseFileOwnership();

    job->catalog_file_path = job->catalog->database_path();
    job->catalog_file_size = GetFileSize(job->catalog->database_path());

    return true;
  }


  bool ReopenCatalog(CatalogJob *job) {
    assert(!job->ignore);
    assert(job->catalog == NULL);

    job->catalog = CatalogTN::AttachFreely(job->path,
                                           job->catalog_file_path,
                                           job->hash,
                                           job->parent,
                                           !job->IsRootCatalog());

    if (job->catalog == NULL) {
      LogCvmfs(kLogCatalogTraversal, error_sink_,
               "failed to re-open catalog %s", job->hash.ToString().c_str());
      return false;
    }

    return true;
  }


  bool CloseCatalog(const bool unlink_db, CatalogJob *job) {
    delete job->catalog;
    job->catalog = NULL;
    if (!job->catalog_file_path.empty() && unlink_db) {
      const int retval = unlink(job->catalog_file_path.c_str());
      if (retval != 0) {
        LogCvmfs(kLogCatalogTraversal, error_sink_, "Failed to unlink %s - %d",
                 job->catalog_file_path.c_str(), errno);
        return false;
      }
    }

    return true;
  }

  shash::Any GetRepositoryRootCatalogHash() {
    // get the manifest of the repository to learn about the entry point or the
    // root catalog of the repository to be traversed
    UniquePtr<manifest::Manifest> manifest;
    const typename ObjectFetcherT::Failures retval =
                                      object_fetcher_->FetchManifest(&manifest);
    if (retval != ObjectFetcherT::kFailOk) {
      LogCvmfs(kLogCatalogTraversal, kLogStderr, "failed to load manifest "
                                                 "(%d - %s)",
                                                 retval, Code2Ascii(retval));
      return shash::Any();
    }

    assert(manifest.IsValid());
    return manifest->catalog_hash();
  }

  /**
   * Checks if a root catalog is below one of the pruning thresholds.
   * Pruning thesholds can be either the catalog's history depth or a timestamp
   * threshold applied to the last modified timestamp of the catalog.
   *
   * @param ctx  traversal context for traversal-specific state
   * @param job  the job defining the current catalog
   * @return     true if either history or timestamp threshold are satisfied
   */
  bool IsBelowPruningThresholds(
    const CatalogJob &job,
    const unsigned history_depth,
    const time_t timestamp_threshold
  ) {
    assert(job.IsRootCatalog());
    assert(job.catalog != NULL);

    const bool h = job.history_depth >= history_depth;
    assert(timestamp_threshold >= 0);
    const bool t =
      catalog_info_shim_->GetLastModified(job.catalog) <
      unsigned(timestamp_threshold);

    return t || h;
  }

  ObjectFetcherT         *object_fetcher_;
  CatalogTraversalInfoShim<CatalogTN> catalog_info_default_shim_;
  CatalogTraversalInfoShim<CatalogTN> *catalog_info_shim_;
  const unsigned int      default_history_depth_;
  const time_t            default_timestamp_threshold_;
  const bool              no_close_;
  const bool              ignore_load_failure_;
  const bool              no_repeat_history_;
  LogFacilities           error_sink_;
};

/**
 * This class traverses the catalog hierarchy of a CVMFS repository recursively.
 * Also historic catalog trees can be traversed. The user needs to specify a
 * callback which is called for each catalog on the way.
 *
 * CatalogTraversal<> can be configured and used in various ways:
 *   -> Historic catalog traversal
 *   -> Prune catalogs below a certain history level
 *   -> Prune catalogs older than a certain threshold timestamp
 *   -> Never traverse a certain catalog twice
 *   -> Breadth First Traversal or Depth First Traversal
 *   -> Optional catalog memory management (no_close)
 *   -> Use all Named Snapshots of a repository as traversal entry point
 *   -> Traverse starting from a provided catalog
 *   -> Traverse catalogs that were previously skipped
 *   -> Produce various flavours of catalogs (writable, mocked, ...)
 *
 * Breadth First Traversal Strategy
 *   Catalogs are handed out to the user identical as they are traversed.
 *   Say: From top to buttom. When you would simply print each received catalog
 *        the result would be a nice representation of the catalog tree.
 *   This method is more efficient, because catalogs are opened, processed and
 *   thrown away directly afterwards.
 *
 * Depth First Traversal Strategy
 *   The user gets the catalog tree starting from the leaf nodes.
 *   Say: From bottom to top. A user can assume that he got all children or
 *        historical ancestors of a catalog before.
 *   This method climbs down the full catalog tree and hands it out 'in reverse
 *   order'. Thus, catalogs on the way are opened, checked for their descendants
 *   and closed. Once all children and historical ancestors are processed, it is
 *   re-opened and handed out to the user.
 *   Note: This method needs more disk space to temporarily store downloaded but
 *         not yet processed catalogs.
 *
 * Note: Since all CVMFS catalog files together can grow to several gigabytes in
 *       file size, each catalog is loaded, processed and removed immediately
 *       afterwards. Except if no_close is specified, which allows the user to
 *       choose when a catalog should be closed. Keep in mind, that a user is
 *       responsible for both deletion of the delivered catalog objects as well
 *       as unlinking of the catalog database file.
 *
 *
 * @param ObjectFetcherT  Strategy Pattern implementation that defines how to
 *                        retrieve catalogs from various backend storage types.
 *                        Furthermore the ObjectFetcherT::CatalogTN is the type
 *                        of catalog to be instantiated by CatalogTraversal<>.
 *
 * CAUTION: the CatalogTN* pointer passed into the callback becomes invalid
 *          directly after the callback method returns, unless you create the
 *          CatalogTraversal object with no_close = true.
 */
template<class ObjectFetcherT>
class CatalogTraversal
  : public CatalogTraversalBase<ObjectFetcherT>
{
 public:
  typedef CatalogTraversalBase<ObjectFetcherT> Base;
  typedef ObjectFetcherT                      ObjectFetcherTN;
  typedef typename ObjectFetcherT::CatalogTN  CatalogTN;
  typedef typename ObjectFetcherT::HistoryTN  HistoryTN;
  typedef CatalogTraversalData<CatalogTN>     CallbackDataTN;
  typedef typename Base::Parameters           Parameters;
  typedef typename Base::TraversalType        TraversalType;
  typedef typename Base::CatalogJob           CatalogJob;

 protected:
  typedef std::set<shash::Any> HashSet;
  typedef std::stack<CatalogJob> CatalogJobStack;

  /**
   * This struct represents a catalog traversal context. It needs to be re-
   * created for each catalog traversal process and contains information to this
   * specific catalog traversal run.
   *
   * @param history_depth   the history traversal threshold
   * @param traversal_type  either breadth or depth first traversal strategy
   * @param catalog_stack   the call stack for catalogs to be traversed
   * @param callback_stack  used in depth first traversal for deferred yielding
   */
  struct TraversalContext {
    TraversalContext(const unsigned       history_depth,
                     const time_t         timestamp_threshold,
                     const TraversalType  traversal_type) :
      history_depth(history_depth),
      timestamp_threshold(timestamp_threshold),
      traversal_type(traversal_type) {}

    const unsigned       history_depth;
    const time_t         timestamp_threshold;
    const TraversalType  traversal_type;
    CatalogJobStack      catalog_stack;
    CatalogJobStack      callback_stack;
  };

 public:
  /**
   * Constructs a new catalog traversal engine based on the construction
   * parameters described in struct ConstructionParams.
   */
  explicit CatalogTraversal(const Parameters &params)
    : CatalogTraversalBase<ObjectFetcherT>(params)
  { }

  bool Traverse(const TraversalType type = Base::kBreadthFirst) {
    const shash::Any root_catalog_hash = this->GetRepositoryRootCatalogHash();
    if (root_catalog_hash.IsNull()) {
      return false;
    }
    return Traverse(root_catalog_hash, type);
  }

  bool Traverse(const shash::Any     &root_catalog_hash,
                const TraversalType   type = Base::kBreadthFirst) {
    // add the root catalog of the repository as the first element on the job
    // stack
    TraversalContext ctx(this->default_history_depth_,
                         this->default_timestamp_threshold_,
                         type);
    Push(root_catalog_hash, &ctx);
    return DoTraverse(&ctx);
  }

  bool TraverseList(const std::vector<shash::Any> &catalog_list,
                    const TraversalType type = Base::kBreadthFirst) {
    std::vector<shash::Any>::const_iterator i = catalog_list.begin();
    const std::vector<shash::Any>::const_iterator iend = catalog_list.end();
    bool success = true;
    for (; i != iend; ++i) {
      success = success && TraverseRevision(*i, type);
    }
    return success;
  }

  bool TraverseRevision(
    const shash::Any     &root_catalog_hash,
    const TraversalType type = Base::kBreadthFirst)
  {
    // add the given root catalog as the first element on the job stack
    TraversalContext ctx(Parameters::kNoHistory,
                         Parameters::kNoTimestampThreshold,
                         type);
    Push(root_catalog_hash, &ctx);
    return DoTraverse(&ctx);
  }

 protected:
  /**
   * This controls the actual traversal. Using a stack to traverse down the
   * catalog hierarchy. This method implements the traversal itself, but not
   * in which way catalogs are handed out to the user code.
   *
   * Each catalog is processed in these steps:
   *  1.) Pop the next catalog from the stack
   *        Catalogs are always traversed from latest to oldest revision and
   *        from root to leaf nested catalogs
   *  2.) Prepare the catalog for traversing
   *    2.1.) Check if it was visited before
   *    2.2.) Fetch the catalog database from the backend storage
   *            This might fail and produce an error. For root catalogs this
   *            error can be ignored (might be garbage collected before)
   *    2.3.) Open the catalog database
   *    2.4.) Check if the catalog is older than the timestamp threshold
   *        After these steps the catalog is opened either opened and ready for
   *        the traversal to continue, or it was marked for ignore (job.ignore)
   *  3.) Check if the catalog is marked to be ignored
   *        Catalog might not be loadable (sweeped root catalog) or is too old
   *        Note: ignored catalogs can still trigger postponed yields
   *  4.) Mark the catalog as visited to be able to skip it later on
   *  5.) Find and push referencing catalogs
   *        This pushes all descendents of the current catalog onto the stack.
   *        Note that this is dependent on the strategy (depth or breadth first)
   *        and on the history threshold (see history_depth).
   *  6.) Hand the catalog out to the user code
   *        Depending on the traversal strategy (depth of breadths first) this
   *        might immediately yield zero to N catalogs to the user code.
   *
   * Note: If anything unexpected goes wrong during the traversal process, it
   *       is aborted immediately.
   *
   * @param ctx   the traversal context that steers the whole traversal process
   * @return      true on successful traversal and false on abort
   */
  bool DoTraverse(TraversalContext *ctx) {
    assert(ctx->callback_stack.empty());

    while (!ctx->catalog_stack.empty()) {
      // Get the top most catalog for the next processing step
      CatalogJob job = Pop(ctx);

      if (ShouldBeSkipped(job)) {
        job.ignore = true;
      // download and open the catalog for processing
      } else if (!this->PrepareCatalog(&job)) {
        return false;
      }

      // ignored catalogs don't need to be processed anymore but they might
      // release postponed yields
      if (job.ignore) {
        if (!HandlePostponedYields(job, ctx)) {
          return false;
        }
        continue;
      }

      // push catalogs referenced by the current catalog (onto stack)
      this->MarkAsVisited(job);
      PushReferencedCatalogs(&job, ctx);

      // notify listeners
      if (!YieldToListeners(&job, ctx)) {
        return false;
      }
    }

    // invariant: after the traversal finshed, there should be no more catalogs
    //            to traverse or to yield!
    assert(ctx->catalog_stack.empty());
    assert(ctx->callback_stack.empty());
    return true;
  }

  void PushReferencedCatalogs(CatalogJob *job, TraversalContext *ctx) {
    assert(!job->ignore);
    assert(job->catalog != NULL);
    assert(ctx->traversal_type == Base::kBreadthFirst ||
           ctx->traversal_type == Base::kDepthFirst);

    // this differs, depending on the traversal strategy.
    //
    // Breadths First Traversal
    //   Catalogs are traversed from top (root catalog) to bottom (leaf
    //   catalogs) and from more recent (HEAD revision) to older (historic
    //   revisions)
    //
    // Depth First Traversal
    //   Catalogs are traversed from oldest revision (depends on the configured
    //   maximal history depth) to the HEAD revision and from bottom (leafs) to
    //   top (root catalogs)
    job->referenced_catalogs =
      (ctx->traversal_type == Base::kBreadthFirst)
      ? PushPreviousRevision(*job, ctx) + PushNestedCatalogs(*job, ctx)
      : PushNestedCatalogs(*job, ctx) + PushPreviousRevision(*job, ctx);
  }

  /**
   * Pushes the previous revision of a (root) catalog.
   * @return  the number of catalogs pushed on the processing stack
   */
  unsigned int PushPreviousRevision(
    const CatalogJob &job,
    TraversalContext *ctx
  ) {
    // only root catalogs are used for entering a previous revision (graph)
    if (!job.catalog->IsRoot()) {
      return 0;
    }

    const shash::Any previous_revision = job.catalog->GetPreviousRevision();
    if (previous_revision.IsNull()) {
      return 0;
    }

    // check if the next deeper history level is actually requested
    // Note: if the current catalog is below the timestamp threshold it will be
    //       traversed and only its ancestor revision will not be pushed anymore
    if (this->IsBelowPruningThresholds(job, ctx->history_depth,
                                       ctx->timestamp_threshold)) {
      return 0;
    }

    Push(CatalogJob("", previous_revision, 0, job.history_depth + 1, NULL),
         ctx);
    return 1;
  }

  /**
   * Pushes all the referenced nested catalogs.
   * @return  the number of catalogs pushed on the processing stack
   */
  unsigned int PushNestedCatalogs(
    const CatalogJob &job,
    TraversalContext *ctx
  ) {
    typedef typename CatalogTN::NestedCatalogList NestedCatalogList;
    const NestedCatalogList nested = job.catalog->ListOwnNestedCatalogs();
    typename NestedCatalogList::const_iterator i    = nested.begin();
    typename NestedCatalogList::const_iterator iend = nested.end();
    for (; i != iend; ++i) {
      CatalogTN* parent = (this->no_close_) ? job.catalog : NULL;
      const CatalogJob new_job(i->mountpoint.ToString(),
                               i->hash,
                               job.tree_level + 1,
                               job.history_depth,
                               parent);
      Push(new_job, ctx);
    }

    return nested.size();
  }

  void Push(const shash::Any &root_catalog_hash, TraversalContext *ctx) {
    Push(CatalogJob("", root_catalog_hash, 0, 0), ctx);
  }

  bool YieldToListeners(CatalogJob *job, TraversalContext *ctx) {
    assert(!job->ignore);
    assert(job->catalog != NULL);
    assert(ctx->traversal_type == Base::kBreadthFirst ||
           ctx->traversal_type == Base::kDepthFirst);

    // in breadth first search mode, every catalog is simply handed out once
    // it is visited. No extra magic required...
    if (ctx->traversal_type == Base::kBreadthFirst) {
      return Yield(job);
    }

    // in depth first search mode, catalogs might need to wait until all of
    // their referenced catalogs are yielded (ctx.callback_stack)...
    assert(ctx->traversal_type == Base::kDepthFirst);
    if (job->referenced_catalogs > 0) {
      PostponeYield(job, ctx);
      return true;
    }

    // this catalog can be yielded
    return Yield(job) && HandlePostponedYields(*job, ctx);
  }

  /**
   * Checks the traversal history if the given catalog was traversed or at least
   * seen before. If 'no_repeat_history' is not set this is always 'false'.
   *
   * @param job   the job to be checked against the traversal history
   * @return      true if the specified catalog was hit before
   */
  bool ShouldBeSkipped(const CatalogJob &job) {
    return this->no_repeat_history_ && (visited_catalogs_.count(job.hash) > 0);
  }

  void MarkAsVisited(const CatalogJob &job) {
    if (this->no_repeat_history_) {
      visited_catalogs_.insert(job.hash);
    }
  }

 private:
  /**
   * This actually hands out a catalog to the user code
   * It is not called by DoTraversa() directly but by wrapper functions in order
   * to provide higher level yielding behaviour.
   */
  bool Yield(CatalogJob *job) {
    assert(!job->ignore);
    assert(job->catalog != NULL || job->postponed);

    // catalog was pushed on ctx.callback_stack before, it might need to be re-
    // opened. If CatalogTraversal<> is configured with no_close, it was not
    // closed before, hence does not need a re-open.
    if (job->postponed && !this->no_close_ && !this->ReopenCatalog(job)) {
      return false;
    }

    // hand the catalog out to the user code (see Observable<>)
    assert(job->catalog != NULL);
    this->NotifyListeners(job->GetCallbackData());

    // skip the catalog closing procedure if asked for
    // Note: In this case it is the user's responsibility to both delete the
    //       yielded catalog object and the underlying database temp file
    if (this->no_close_) {
      return true;
    }

    // we can close the catalog here and delete the temporary file
    const bool unlink_db = true;
    return this->CloseCatalog(unlink_db, job);
  }


  /**
   * Pushes a catalog to the callback_stack for later yielding
   * Note: this is only used for the Depth First Traversal strategy!
   */
  void PostponeYield(CatalogJob *job, TraversalContext *ctx) {
    assert(job->referenced_catalogs > 0);

    job->postponed = true;
    if (!this->no_close_) {
      const bool unlink_db = false;  // will reopened just before yielding
      this->CloseCatalog(unlink_db, job);
    }
    ctx->callback_stack.push(*job);
  }

  /**
   * Determines if there are postponed yields that can be set free based on
   * the catalog currently being yielded
   *
   * Note: the CatalogJob being handed into this method does not necessarily
   *       have an open Catalog attached to it.
   *
   * @param ctx   the TraversalContext
   * @param job   the catalog job that was just yielded
   * @return      true on successful execution
   */
  bool HandlePostponedYields(const CatalogJob &job, TraversalContext *ctx) {
    if (ctx->traversal_type == Base::kBreadthFirst) {
      return true;
    }

    assert(ctx->traversal_type == Base::kDepthFirst);
    assert(job.referenced_catalogs == 0);

    // walk through the callback_stack and yield all catalogs that have no un-
    // yielded referenced_catalogs anymore. Every time a CatalogJob in the
    // callback_stack gets yielded it decrements the referenced_catalogs of the
    // next top of the stack (it's parent CatalogJob waiting for yielding)
    CatalogJobStack &clbs = ctx->callback_stack;
    while (!clbs.empty()) {
      CatalogJob &postponed_job = clbs.top();
      if (--postponed_job.referenced_catalogs > 0) {
        break;
      }

      if (!Yield(&postponed_job)) {
        return false;
      }
      clbs.pop();
    }

    return true;
  }

  void Push(const CatalogJob &job, TraversalContext *ctx) {
    ctx->catalog_stack.push(job);
  }

  CatalogJob Pop(TraversalContext *ctx) {
    CatalogJob job = ctx->catalog_stack.top();
    ctx->catalog_stack.pop();
    return job;
  }

 protected:
  HashSet                 visited_catalogs_;
};

template <class ObjectFetcherT>
const unsigned int
  CatalogTraversalBase<ObjectFetcherT>::Parameters::kFullHistory =
    std::numeric_limits<unsigned int>::max();

template <class ObjectFetcherT>
const unsigned int
  CatalogTraversalBase<ObjectFetcherT>::Parameters::kNoHistory = 0;

template <class ObjectFetcherT>
const time_t
  CatalogTraversalBase<ObjectFetcherT>::Parameters::kNoTimestampThreshold = 0;

}  // namespace swissknife

#endif  // CVMFS_CATALOG_TRAVERSAL_H_
