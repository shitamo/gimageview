/* -*- Mode: C++; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * gimv-faces: finds the faces in the images of the index (OpenCV YuNet),
 * keeps a feature of each (OpenCV SFace) and groups the faces that look
 * alike, for gimv to show as candidates for a person to name.
 *
 * Run by gimv when the indexer has nothing to do; it works through the
 * images of the given folders that are new or changed since last time and
 * exits.  The folders must be in the index (the files come from it).
 *
 *   gimv-faces --db FILE --models DIR [--regroup] [--export DIR] [--] FOLDER...
 *   gimv-faces --evaluate --db FILE
 *   gimv-faces --group-only --db FILE
 *   gimv-faces --add-face X,Y,W,H --db FILE --models DIR IMAGE
 *
 * --regroup: make the groups again even when no faces changed
 * --export:  write the faces of the groups as small images to DIR, to look at
 * --evaluate: the ways of grouping tried on the faces named (as if they had
 *            no name), the names telling how right the groups are; the
 *            database is only read
 * --group-only: only the groups and guesses made again (no images read,
 *            no models needed)
 * --add-face: a face a person marked on the image (X, Y, W, H in the pixels
 *            of the image as shown), looked for again there enlarged (a
 *            small face, the eyes for the feature), else taken as marked;
 *            kept when the image is read again (face_manual).  Says
 *            "added <id> <1: found again, 0: as marked>" or "exists <id>"
 *
 * Output (one line each):
 *   start <todo> <total>          files to read, files in the folders
 *   progress <done> <todo>
 *   grouping <faces>
 *   done <files read> <faces found> <groups>
 *   no-models <dir>               the model files are not there
 *   busy                          another gimv-faces runs
 *   error <message>
 * It stops when stdin is closed (gimv has gone) or reads "quit".
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <sqlite3.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#define DB_VERSION        5
#define DETECT_MODEL      "face_detection_yunet_2023mar.onnx"
#define RECOGNIZE_MODEL   "face_recognition_sface_2021dec.onnx"

/* the long side of the image the faces are looked for in */
#define DETECT_SIZE       1600
/* faces smaller than this (pixels in that image) are too blurred to tell */
#define MIN_FACE          32
/* a second look in a small image for a face filling much of the photo
 * (a close-up): YuNet misses faces of more than about 400 pixels in the
 * image it is given */
#define CLOSE_SIZE        320
/* those smaller than this (pixels in that image) are found by the first */
#define CLOSE_MIN         64
/* a face of the second look over one of the first (this part of the
 * smaller one): the same face */
#define SAME_FACE         0.5
#define SCORE_MIN         0.85f
#define FEATURE_LEN       128
/* cosine similarity: SFace's own threshold for "the same person" is 0.363;
 * groups are made a bit stricter to keep them clean */
#define GROUP_SIM         0.46f   /* 0.42 before 2026-11 (--evaluate: E) */
#define GUESS_SIM         0.45f
#define COMMIT_USEC       (3 * 1000000LL)

static const char *exts[] = {
   "jpg", "jpeg", "jpe", "png", "webp", "bmp", "tif", "tiff", nullptr
};


/******************************************************************************
 *  talking to gimv
 ******************************************************************************/

static bool output_gone = false;
static bool quit = false;

static void
say (const char *fmt, ...)
{
   va_list ap;

   if (output_gone) return;
   va_start (ap, fmt);
   vfprintf (stdout, fmt, ap);
   va_end (ap);
   fputc ('\n', stdout);
   if (fflush (stdout) != 0 || ferror (stdout))
      output_gone = true;
}


static void
check_stdin (void)
{
   struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
   char buf[256];

   if (output_gone) quit = true;
   while (!quit && poll (&pfd, 1, 0) > 0) {
      ssize_t n = read (STDIN_FILENO, buf, sizeof (buf) - 1);
      if (n <= 0) {
         if (n < 0 && errno == EINTR) continue;
         quit = true;
         return;
      }
      buf[n] = '\0';
      if (strstr (buf, "quit")) quit = true;
   }
}


static long long
now_usec (void)
{
   struct timespec ts;
   clock_gettime (CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}


static void
lower_priority (void)
{
   setpriority (PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
   syscall (SYS_ioprio_set, 1 /* who: process */, 0, 3 << 13 /* class: idle */);
#endif
}


/******************************************************************************
 *  database
 ******************************************************************************/

struct Db {
   sqlite3 *db = nullptr;

   ~Db () { if (db) sqlite3_close (db); }

   sqlite3_stmt *prep (const char *sql)
   {
      sqlite3_stmt *st = nullptr;
      if (sqlite3_prepare_v2 (db, sql, -1, &st, nullptr) != SQLITE_OK) {
         say ("error prepare: %s", sqlite3_errmsg (db));
         return nullptr;
      }
      return st;
   }

   void exec (const char *sql)
   {
      char *err = nullptr;
      if (sqlite3_exec (db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
         say ("error %s: %s", sql, err ? err : "?");
         sqlite3_free (err);
      }
   }
};


static int
busy_poll (void *, int count)
{
   if (count >= 60 * 500) return 0;   /* a minute */
   usleep (2000);
   return 1;
}


static bool
open_db (Db &d, const char *path)
{
   sqlite3_stmt *st;
   int v = -1;

   /* the indexer makes it and its schema: only an existing one is used */
   if (sqlite3_open_v2 (path, &d.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
      say ("error cannot open %s", path);
      return false;
   }
   /* the indexer and gimv commit often but take the lock again at once:
    * looked at every few milliseconds (SQLite's own wait, growing to
    * 100 ms sleeps, mostly misses the gaps) */
   sqlite3_busy_handler (d.db, busy_poll, nullptr);
   st = d.prep ("PRAGMA user_version");
   if (st && sqlite3_step (st) == SQLITE_ROW)
      v = sqlite3_column_int (st, 0);
   sqlite3_finalize (st);
   if (v != DB_VERSION) {
      say ("error the index has version %d, not %d", v, DB_VERSION);
      return false;
   }
   d.exec ("PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;");
   /* the files looked at for close-ups too (the files looked at before
    * that get the second look alone, their faces kept); made here, not by
    * the indexer: no new version of the database */
   d.exec ("CREATE TABLE IF NOT EXISTS face_closeup ("
           "  file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE)");
   /* the faces a person marked (--add-face): kept when the image is read
    * again */
   d.exec ("CREATE TABLE IF NOT EXISTS face_manual ("
           "  face_id INTEGER PRIMARY KEY REFERENCES faces(id) ON DELETE CASCADE)");
   return true;
}


/* "dir" itself or below it */
static bool
under (const std::string &path, const std::vector<std::string> &dirs)
{
   for (const auto &d : dirs) {
      if (path == d) return true;
      if (path.size () > d.size () && path.compare (0, d.size (), d) == 0
          && (d.back () == '/' || path[d.size ()] == '/'))
         return true;
   }
   return false;
}


static bool
wanted_ext (const char *name)
{
   const char *dot = strrchr (name, '.');
   if (!dot) return false;
   for (int i = 0; exts[i]; i++)
      if (!strcasecmp (dot + 1, exts[i])) return true;
   return false;
}


struct Face;

struct Todo {
   long long   id;
   long long   mtime;
   std::string path;
   /* only the thumbnails of these faces (found before thumbnails were made) */
   std::vector<Face> *missing = nullptr;
   /* looked at before close-ups were: only the second look, the faces of
    * the file kept */
   bool closeup = false;
};


/* the files of the folders not looked at yet (or changed since); the faces
 * of files no longer in the folders are dropped */
static void
list_files (Db &d, const std::vector<std::string> &dirs,
            std::vector<Todo> &todo, long long &total)
{
   sqlite3_stmt *st;
   std::vector<long long> gone;

   std::set<long long> seen;

   /* through the index of dirs.path: only the rows of these folders */
   std::vector<Todo> closeups;

   st = d.prep ("SELECT f.id, f.mtime, d.path, f.name, s.mtime, s.n, c.file_id "
                "FROM dirs d JOIN files f ON f.dir_id = d.id "
                "LEFT JOIN face_scans s ON s.file_id = f.id "
                "LEFT JOIN face_closeup c ON c.file_id = f.id "
                "WHERE d.path = ?1 OR (d.path >= ?2 AND d.path < ?3)");
   total = 0;
   for (const auto &dir : dirs) {
      std::string below = dir + "/", after = dir + "0";   /* '0' follows '/' */
      sqlite3_bind_text (st, 1, dir.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (st, 2, below.c_str (), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text (st, 3, after.c_str (), -1, SQLITE_TRANSIENT);
      while (st && sqlite3_step (st) == SQLITE_ROW) {
         const char *dpath = (const char *) sqlite3_column_text (st, 2);
         const char *name  = (const char *) sqlite3_column_text (st, 3);
         long long id = sqlite3_column_int64 (st, 0);
         bool scanned = sqlite3_column_type (st, 4) != SQLITE_NULL;

         if (!dpath || !name || !wanted_ext (name)) continue;
         if (!seen.insert (id).second) continue;   /* folders inside others */
         total++;
         bool same = scanned && sqlite3_column_int64 (st, 4) == sqlite3_column_int64 (st, 1);
         bool closeup = same && sqlite3_column_int (st, 5) >= 0
                        && sqlite3_column_type (st, 6) == SQLITE_NULL;
         if (same && !closeup) continue;
         std::string path (dpath);
         if (path.back () != '/') path += '/';
         path += name;
         if (closeup) {
            closeups.push_back ({ id, sqlite3_column_int64 (st, 1), path });
            closeups.back ().closeup = true;
         } else {
            todo.push_back ({ id, sqlite3_column_int64 (st, 1), path });
         }
      }
      sqlite3_reset (st);
   }
   sqlite3_finalize (st);
   /* the new and changed files first */
   for (Todo &t : closeups) todo.push_back (std::move (t));

   /* the files looked at before that are no longer in the folders */
   st = d.prep ("SELECT s.file_id, d.path, f.name FROM face_scans s "
                "JOIN files f ON f.id = s.file_id JOIN dirs d ON d.id = f.dir_id");
   while (st && sqlite3_step (st) == SQLITE_ROW) {
      const char *dpath = (const char *) sqlite3_column_text (st, 1);
      const char *name  = (const char *) sqlite3_column_text (st, 2);
      if (!dpath || !name || !under (dpath, dirs) || !wanted_ext (name))
         gone.push_back (sqlite3_column_int64 (st, 0));
   }
   sqlite3_finalize (st);

   if (!gone.empty ()) {
      sqlite3_stmt *a = d.prep ("DELETE FROM faces WHERE file_id = ?");
      sqlite3_stmt *b = d.prep ("DELETE FROM face_scans WHERE file_id = ?");
      d.exec ("BEGIN");
      for (long long id : gone) {
         sqlite3_bind_int64 (a, 1, id); sqlite3_step (a); sqlite3_reset (a);
         sqlite3_bind_int64 (b, 1, id); sqlite3_step (b); sqlite3_reset (b);
      }
      d.exec ("COMMIT");
      sqlite3_finalize (a);
      sqlite3_finalize (b);
   }
}


/******************************************************************************
 *  crash guard: the id of the file being read, in FILE.faces-reading
 ******************************************************************************/

static int guard_fd = -1;

static void
guard_open (Db &d, const std::string &dbfile)
{
   std::string g = dbfile + ".faces-reading";
   long long id = 0;
   char buf[32] = { 0 };
   int fd = open (g.c_str (), O_RDONLY | O_CLOEXEC);

   /* the last run died on this one: do not read it again */
   if (fd >= 0) {
      if (read (fd, buf, sizeof (buf) - 1) > 0)
         id = atoll (buf);
      close (fd);
   }
   if (id > 0) {
      /* the second look alone (a file looked at before): its faces kept */
      sqlite3_stmt *st = d.prep ("INSERT OR IGNORE INTO face_closeup (file_id) "
                                 "SELECT s.file_id FROM face_scans s JOIN files f ON f.id = s.file_id "
                                 "WHERE s.file_id = ? AND s.n >= 0 AND s.mtime = f.mtime");
      sqlite3_bind_int64 (st, 1, id);
      sqlite3_step (st);
      sqlite3_finalize (st);
      if (!sqlite3_changes (d.db)) {
         st = d.prep ("INSERT OR REPLACE INTO face_scans (file_id, mtime, n) "
                      "SELECT id, mtime, -1 FROM files WHERE id = ?");
         sqlite3_bind_int64 (st, 1, id);
         sqlite3_step (st);
         sqlite3_finalize (st);
      }
      say ("skipped %lld", id);
   }
   guard_fd = open (g.c_str (), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}


static void
guard_set (long long id)
{
   char buf[32];
   if (guard_fd < 0) return;
   int n = snprintf (buf, sizeof (buf), "%-20lld\n", id);
   if (pwrite (guard_fd, buf, n, 0) < 0) { /* nothing to do */ }
}


static void
guard_close (const std::string &dbfile)
{
   if (guard_fd >= 0) close (guard_fd);
   unlink ((dbfile + ".faces-reading").c_str ());
}


/******************************************************************************
 *  finding the faces
 ******************************************************************************/

struct Face {
   long long id = 0;          /* backfill of the thumbnail: the face */
   int   x, y, w, h;
   float score;
   std::vector<float> feature;
   std::vector<uchar> thumb;  /* JPEG */
};


#define THUMB_SIZE 128

/* a square around the face (and some of the head), as a small JPEG;
 * img is the image scaled down by "scale" from the coordinates */
static void
make_thumb (const cv::Mat &img, double scale, Face &f)
{
   double cx = (f.x + f.w / 2.0) / scale, cy = (f.y + f.h / 2.0) / scale;
   int side = (int) (std::max (f.w, f.h) * 1.6 / scale);
   cv::Mat crop, out;

   side = std::min (side, std::min (img.cols, img.rows));
   if (side < 8) return;
   /* moved inside the image rather than cut */
   int x = std::clamp ((int) (cx - side / 2.0), 0, img.cols - side);
   int y = std::clamp ((int) (cy - side / 2.0), 0, img.rows - side);
   cv::resize (img (cv::Rect (x, y, side, side)), out, cv::Size (THUMB_SIZE, THUMB_SIZE),
               0, 0, side > THUMB_SIZE ? cv::INTER_AREA : cv::INTER_LINEAR);
   try {
      cv::imencode (".jpg", out, f.thumb, { cv::IMWRITE_JPEG_QUALITY, 85 });
   } catch (const cv::Exception &) {
      f.thumb.clear ();
   }
}


/* the image as shown (EXIF orientation applied); scale: how much smaller
 * than the real size it was read */
static bool
load_image (const std::string &path, cv::Mat &img, double &scale)
{
   int flags = cv::IMREAD_COLOR;   /* the EXIF orientation is applied */
   struct stat sb;

   scale = 1.0;
   /* big JPEG files: decoded at half or a quarter, much faster */
   if (stat (path.c_str (), &sb) == 0) {
      const char *dot = strrchr (path.c_str (), '.');
      bool jpeg = dot && (!strcasecmp (dot, ".jpg") || !strcasecmp (dot, ".jpeg")
                          || !strcasecmp (dot, ".jpe"));
      if (jpeg && sb.st_size > 6 * 1024 * 1024) flags = cv::IMREAD_REDUCED_COLOR_4;
      else if (jpeg && sb.st_size > 2 * 1024 * 1024) flags = cv::IMREAD_REDUCED_COLOR_2;
   }
   try {
      img = cv::imread (path, flags);
   } catch (const cv::Exception &) {
      return false;
   }
   if (img.empty ()) return false;
   if (flags == cv::IMREAD_REDUCED_COLOR_4) scale = 4.0;
   else if (flags == cv::IMREAD_REDUCED_COLOR_2) scale = 2.0;
   return true;
}


static void
normalize (std::vector<float> &v)
{
   double s = 0;
   for (float f : v) s += (double) f * f;
   s = std::sqrt (s);
   if (s > 0)
      for (float &f : v) f = (float) (f / s);
}


/* "small" padded to sides of a multiple of 32, as YuNet wants: at the
 * right and bottom, which moves nothing (OpenCV 4.8 and later pad by
 * themselves, this keeps it the same for every version) */
static cv::Mat
pad32 (const cv::Mat &small)
{
   cv::Mat padded;
   int pw = (32 - small.cols % 32) % 32, ph = (32 - small.rows % 32) % 32;
   if (pw || ph)
      cv::copyMakeBorder (small, padded, 0, ph, 0, pw, cv::BORDER_CONSTANT, cv::Scalar (0, 0, 0));
   else
      padded = small;
   return padded;
}


/* this part of the smaller of the two faces is in the other */
static double
overlap (const Face &a, const Face &b)
{
   int x0 = std::max (a.x, b.x), y0 = std::max (a.y, b.y);
   int x1 = std::min (a.x + a.w, b.x + b.w), y1 = std::min (a.y + a.h, b.y + b.h);
   double inter = (double) std::max (0, x1 - x0) * std::max (0, y1 - y0);
   double smaller = std::min ((double) a.w * a.h, (double) b.w * b.h);
   return smaller > 0 ? inter / smaller : 0;
}


/* the faces in "small" (the image scaled down by "scale" from the
 * coordinates of the faces) of "min" pixels or more there, not over one
 * of "avoid"; the thumbnails from "big" (scaled down by "big_scale");
 * false: the models fail */
static bool
detect (const cv::Mat &small, double scale, int min,
        const cv::Mat &big, double big_scale, const std::vector<Face> &avoid,
        cv::Ptr<cv::FaceDetectorYN> &det, cv::Ptr<cv::FaceRecognizerSF> &rec,
        std::vector<Face> &out)
{
   cv::Mat padded = pad32 (small), found;

   try {
      det->setInputSize (padded.size ());
      det->detect (padded, found);
   } catch (const cv::Exception &e) {
      say ("error finding faces: %s", e.what ());
      return false;
   }

   for (int i = 0; i < found.rows; i++) {
      const float *r = found.ptr<float> (i);
      float score = r[14];
      cv::Mat aligned, feat;

      if (score < SCORE_MIN || r[2] < min || r[3] < min) continue;

      Face f;
      f.x = (int) std::lround (std::max (0.0f, r[0]) * scale);
      f.y = (int) std::lround (std::max (0.0f, r[1]) * scale);
      f.w = (int) std::lround (r[2] * scale);
      f.h = (int) std::lround (r[3] * scale);
      f.score = score;
      bool known = false;
      for (const Face &a : avoid)
         if (overlap (a, f) > SAME_FACE) { known = true; break; }
      if (known) continue;

      try {
         rec->alignCrop (padded, found.row (i), aligned);
         rec->feature (aligned, feat);
      } catch (const cv::Exception &) {
         continue;
      }
      if (feat.total () != FEATURE_LEN) continue;
      f.feature.assign ((const float *) feat.data, (const float *) feat.data + FEATURE_LEN);
      normalize (f.feature);
      make_thumb (big, big_scale, f);
      out.push_back (std::move (f));
   }
   return true;
}


/* 1: read, 0: not an image that can be read, -1: the models fail;
 * closeup: only the second look (the faces of "known" kept) */
static int
read_faces (const std::string &path,
            cv::Ptr<cv::FaceDetectorYN> &det, cv::Ptr<cv::FaceRecognizerSF> &rec,
            bool closeup, const std::vector<Face> &known, std::vector<Face> &out)
{
   cv::Mat img, small;
   double scale, small_scale;

   if (!load_image (path, img, scale)) return 0;
   if (img.cols < MIN_FACE * 2 || img.rows < MIN_FACE * 2) return 1;

   int longer = std::max (img.cols, img.rows);
   small_scale = scale;
   if (longer > DETECT_SIZE) {
      double f = (double) DETECT_SIZE / longer;
      cv::resize (img, small, cv::Size (), f, f, cv::INTER_AREA);
      small_scale = scale / f;
   } else {
      small = img;
   }

   /* the faces of the photo */
   if (!closeup
       && !detect (small, small_scale, MIN_FACE, small, small_scale, {}, det, rec, out))
      return -1;

   /* a face too big for that: in a small image (a small photo was looked
    * at as small as that already) */
   if (longer > CLOSE_SIZE * 3 / 2) {
      cv::Mat tiny;
      double f = (double) CLOSE_SIZE / longer;
      std::vector<Face> avoid = closeup ? known : out;
      cv::resize (img, tiny, cv::Size (), f, f, cv::INTER_AREA);
      if (!detect (tiny, scale / f, CLOSE_MIN, small, small_scale, avoid, det, rec, out))
         return -1;
   }
   return 1;
}


struct Result {
   const Todo       *todo = nullptr;
   int               ok = 0;
   std::vector<Face> faces;
};


static void
put_thumb (sqlite3_stmt *st, long long face_id, const std::vector<uchar> &jpeg)
{
   static const uchar none = 0;

   sqlite3_bind_int64 (st, 1, face_id);
   sqlite3_bind_blob (st, 2, jpeg.empty () ? &none : jpeg.data (), (int) jpeg.size (),
                      SQLITE_STATIC);
   sqlite3_step (st);
   sqlite3_reset (st);
}


/* the faces found before the thumbnails were made (database version 3):
 * their thumbnails, a file at a time, after the files to read */
static void
list_missing_thumbs (Db &d, std::vector<Todo> &todo, std::vector<std::vector<Face>> &missing)
{
   std::set<long long> reading;
   for (const Todo &t : todo) reading.insert (t.id);

   sqlite3_stmt *st = d.prep (
      "SELECT fa.id, fa.file_id, fa.x, fa.y, fa.w, fa.h, f.mtime, d.path, f.name "
      "FROM faces fa LEFT JOIN face_thumbs t ON t.face_id = fa.id "
      "JOIN files f ON f.id = fa.file_id JOIN dirs d ON d.id = f.dir_id "
      "WHERE t.face_id IS NULL ORDER BY fa.file_id");
   std::vector<Todo> more;
   while (st && sqlite3_step (st) == SQLITE_ROW) {
      long long file = sqlite3_column_int64 (st, 1);
      if (reading.count (file)) continue;
      if (more.empty () || more.back ().id != file) {
         std::string path ((const char *) sqlite3_column_text (st, 7));
         if (path.back () != '/') path += '/';
         path += (const char *) sqlite3_column_text (st, 8);
         more.push_back ({ file, sqlite3_column_int64 (st, 6), path });
         missing.emplace_back ();
      }
      Face f;
      f.id = sqlite3_column_int64 (st, 0);
      f.x = sqlite3_column_int (st, 2);
      f.y = sqlite3_column_int (st, 3);
      f.w = sqlite3_column_int (st, 4);
      f.h = sqlite3_column_int (st, 5);
      f.score = 0;
      missing.back ().push_back (f);
   }
   sqlite3_finalize (st);
   /* pointers into "missing" only now, when it does not grow any more */
   for (size_t i = 0; i < more.size (); i++) {
      more[i].missing = &missing[i];
      todo.push_back (more[i]);
   }
}


static void
write_results (Db &d, std::vector<Result> &results)
{
   if (results.empty ()) return;

   /* the faces a person marked stay */
   sqlite3_stmt *del  = d.prep ("DELETE FROM faces WHERE file_id = ? "
                                "AND id NOT IN (SELECT face_id FROM face_manual)");
   sqlite3_stmt *kept = d.prep ("SELECT fa.x, fa.y, fa.w, fa.h FROM faces fa "
                                "JOIN face_manual m ON m.face_id = fa.id WHERE fa.file_id = ?");
   sqlite3_stmt *ins  = d.prep ("INSERT INTO faces (file_id, x, y, w, h, score, feature) "
                                "VALUES (?, ?, ?, ?, ?, ?, ?)");
   sqlite3_stmt *scan = d.prep ("INSERT OR REPLACE INTO face_scans (file_id, mtime, n) "
                                "VALUES (?, ?, ?)");

   sqlite3_stmt *thumb = d.prep ("INSERT OR REPLACE INTO face_thumbs (face_id, jpeg) "
                                 "VALUES (?, ?)");
   sqlite3_stmt *closeup_st = d.prep ("INSERT OR IGNORE INTO face_closeup (file_id) VALUES (?)");
   sqlite3_stmt *more  = d.prep ("UPDATE face_scans SET n = n + ? WHERE file_id = ? AND n >= 0");

   if (sqlite3_exec (d.db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
      /* kept for the next time */
      sqlite3_finalize (del);
      sqlite3_finalize (kept);
      sqlite3_finalize (ins);
      sqlite3_finalize (scan);
      sqlite3_finalize (thumb);
      sqlite3_finalize (closeup_st);
      sqlite3_finalize (more);
      return;
   }
   for (const Result &r : results) {
      if (r.todo->missing) {
         /* an empty one when the image cannot be read: not tried again */
         for (const Face &f : r.faces)
            put_thumb (thumb, f.id, f.thumb);
         continue;
      }
      /* a second look alone: the faces found before kept */
      if (!r.todo->closeup) {
         sqlite3_bind_int64 (del, 1, r.todo->id);
         sqlite3_step (del);
         sqlite3_reset (del);
      }
      /* the faces marked by a person: not found twice */
      std::vector<Face> manual;
      sqlite3_bind_int64 (kept, 1, r.todo->id);
      while (sqlite3_step (kept) == SQLITE_ROW) {
         Face m;
         m.x = sqlite3_column_int (kept, 0);
         m.y = sqlite3_column_int (kept, 1);
         m.w = sqlite3_column_int (kept, 2);
         m.h = sqlite3_column_int (kept, 3);
         manual.push_back (std::move (m));
      }
      sqlite3_reset (kept);
      int n_new = 0;
      /* the file may have gone from the index meanwhile: then these fail
       * (foreign key), which is right */
      for (const Face &f : r.faces) {
         bool twice = false;
         for (const Face &m : manual)
            if (overlap (m, f) > SAME_FACE) { twice = true; break; }
         if (twice) continue;
         n_new++;
         sqlite3_bind_int64 (ins, 1, r.todo->id);
         sqlite3_bind_int (ins, 2, f.x);
         sqlite3_bind_int (ins, 3, f.y);
         sqlite3_bind_int (ins, 4, f.w);
         sqlite3_bind_int (ins, 5, f.h);
         sqlite3_bind_double (ins, 6, f.score);
         sqlite3_bind_blob (ins, 7, f.feature.data (), FEATURE_LEN * sizeof (float),
                            SQLITE_STATIC);
         if (sqlite3_step (ins) == SQLITE_DONE)
            put_thumb (thumb, sqlite3_last_insert_rowid (d.db), f.thumb);
         sqlite3_reset (ins);
      }
      if (r.todo->closeup) {
         sqlite3_bind_int (more, 1, n_new);
         sqlite3_bind_int64 (more, 2, r.todo->id);
         sqlite3_step (more);
         sqlite3_reset (more);
      } else {
         sqlite3_bind_int64 (scan, 1, r.todo->id);
         sqlite3_bind_int64 (scan, 2, r.todo->mtime);
         sqlite3_bind_int (scan, 3, r.ok > 0 ? n_new + (int) manual.size () : -1);
         sqlite3_step (scan);
         sqlite3_reset (scan);
      }
      sqlite3_bind_int64 (closeup_st, 1, r.todo->id);
      sqlite3_step (closeup_st);
      sqlite3_reset (closeup_st);
   }
   d.exec ("COMMIT");
   sqlite3_finalize (del);
   sqlite3_finalize (kept);
   sqlite3_finalize (ins);
   sqlite3_finalize (scan);
   sqlite3_finalize (thumb);
   sqlite3_finalize (closeup_st);
   sqlite3_finalize (more);
   results.clear ();
}


/******************************************************************************
 *  groups
 ******************************************************************************/

static float
dot (const float *a, const float *b)
{
   float s = 0;
   for (int i = 0; i < FEATURE_LEN; i++) s += a[i] * b[i];
   return s;
}


struct Row {
   long long id;
   float     score;
   long long person;   /* 0: none */
   bool      ignored;
   int       cluster;   /* before: 0 none */
   std::vector<float> f;
};


/* the unnamed faces: the person they look like (the mean of the faces
 * named so), else a group of faces that look alike (leader clustering in
 * the order of the clearest faces first, against the mean of each group) */
static int
make_groups (Db &d)
{
   std::vector<Row> rows;
   sqlite3_stmt *st = d.prep ("SELECT id, score, person_id, feature, ignored, cluster FROM faces");

   while (st && sqlite3_step (st) == SQLITE_ROW) {
      const void *blob = sqlite3_column_blob (st, 3);
      if (sqlite3_column_bytes (st, 3) != FEATURE_LEN * (int) sizeof (float)) continue;
      Row r;
      r.id = sqlite3_column_int64 (st, 0);
      r.score = (float) sqlite3_column_double (st, 1);
      r.person = sqlite3_column_int64 (st, 2);
      r.ignored = sqlite3_column_int (st, 4) != 0;
      r.cluster = sqlite3_column_int (st, 5);
      r.f.assign ((const float *) blob, (const float *) blob + FEATURE_LEN);
      rows.push_back (std::move (r));
   }
   sqlite3_finalize (st);
   say ("grouping %zu", rows.size ());

   /* "not this person" */
   std::map<long long, std::set<long long>> rejects;
   st = d.prep ("SELECT face_id, person_id FROM face_rejects");
   while (st && sqlite3_step (st) == SQLITE_ROW)
      rejects[sqlite3_column_int64 (st, 0)].insert (sqlite3_column_int64 (st, 1));
   sqlite3_finalize (st);

   /* the persons */
   std::map<long long, std::vector<float>> persons;
   for (const Row &r : rows) {
      if (!r.person || r.ignored) continue;
      auto &m = persons[r.person];
      if (m.empty ()) m.assign (FEATURE_LEN, 0.0f);
      for (int i = 0; i < FEATURE_LEN; i++) m[i] += r.f[i];
   }
   for (auto &p : persons) normalize (p.second);

   std::vector<size_t> order;
   for (size_t i = 0; i < rows.size (); i++)
      if (!rows[i].person && !rows[i].ignored) order.push_back (i);
   std::sort (order.begin (), order.end (),
              [&] (size_t a, size_t b) { return rows[a].score > rows[b].score; });

   std::vector<long long> guess (rows.size (), 0);
   std::vector<int> group (rows.size (), -1);
   std::vector<std::vector<float>> sums, means;
   std::vector<int> sizes;

   for (size_t k = 0; k < order.size (); k++) {
      size_t i = order[k];
      float best = GUESS_SIM;
      auto rej = rejects.find (rows[i].id);
      for (const auto &p : persons) {
         if (rej != rejects.end () && rej->second.count (p.first)) continue;
         float s = dot (rows[i].f.data (), p.second.data ());
         if (s >= best) { best = s; guess[i] = p.first; }
      }
      if (guess[i]) continue;

      int g = -1;
      best = GROUP_SIM;
      for (size_t c = 0; c < means.size (); c++) {
         float s = dot (rows[i].f.data (), means[c].data ());
         if (s >= best) { best = s; g = (int) c; }
      }
      if (g < 0) {
         g = (int) means.size ();
         sums.emplace_back (FEATURE_LEN, 0.0f);
         means.emplace_back ();
         sizes.push_back (0);
      }
      for (int j = 0; j < FEATURE_LEN; j++) sums[g][j] += rows[i].f[j];
      means[g] = sums[g];
      normalize (means[g]);
      sizes[g]++;
      group[i] = g;
      if ((k & 1023) == 0) {
         check_stdin ();
         if (quit) return 0;
      }
   }

   /* each face of the groups again into the group whose mean it is most
    * like (or alone), till nothing moves: the first faces of a group no
    * longer decide it (--evaluate: "wrong" 574 -> 389 with GROUP_SIM 0.46
    * on the faces a user had named) */
   for (int round = 0; round < 10 && !quit; round++) {
      int n = (int) means.size ();
      std::vector<std::vector<float>> m (n, std::vector<float> (FEATURE_LEN, 0.0f));
      std::vector<int> cnt (n, 0);
      for (size_t i = 0; i < rows.size (); i++) {
         if (group[i] < 0) continue;
         for (int j = 0; j < FEATURE_LEN; j++) m[group[i]][j] += rows[i].f[j];
         cnt[group[i]]++;
      }
      for (auto &v : m) normalize (v);
      bool moved = false;
      for (size_t i = 0; i < rows.size (); i++) {
         if (group[i] < 0) continue;
         int g = -1;
         float best = GROUP_SIM;
         for (int c = 0; c < n; c++) {
            /* a face alone is not drawn to its own mean */
            if (cnt[c] == 0 || (cnt[c] < 2 && c == group[i])) continue;
            float sim = dot (rows[i].f.data (), m[c].data ());
            if (sim >= best) { best = sim; g = c; }
         }
         if (g < 0) {
            if (cnt[group[i]] < 2) continue;   /* alone already */
            g = (int) means.size ();
            means.emplace_back (FEATURE_LEN, 0.0f);
            sums.emplace_back (FEATURE_LEN, 0.0f);
            sizes.push_back (0);
            m.emplace_back (FEATURE_LEN, 0.0f);
            cnt.push_back (0);
         }
         if (g != group[i]) { group[i] = g; moved = true; }
      }
      check_stdin ();
      if (!moved) break;
   }
   std::fill (sizes.begin (), sizes.end (), 0);
   for (size_t i = 0; i < rows.size (); i++)
      if (group[i] >= 0) sizes[group[i]]++;

   /* the numbers stay: a group gets the number most of its faces had
    * (the faces window shows a group by its number while names are given
    * to the others); new groups get new numbers.  Groups of one are none. */
   std::map<std::pair<int, int>, int> overlap;   /* (group, old) -> faces */
   int max_old = 0;
   for (size_t i = 0; i < rows.size (); i++) {
      max_old = std::max (max_old, rows[i].cluster);
      if (group[i] >= 0 && rows[i].cluster > 0 && sizes[group[i]] >= 2)
         overlap[{ group[i], rows[i].cluster }]++;
   }
   std::vector<std::pair<int, std::pair<int, int>>> pairs;   /* count, (group, old) */
   for (const auto &o : overlap) pairs.push_back ({ o.second, o.first });
   std::sort (pairs.begin (), pairs.end (),
              [] (const auto &a, const auto &b) { return a.first > b.first; });
   std::vector<int> number (sizes.size (), 0);
   std::set<int> taken;
   for (const auto &p : pairs) {
      int g = p.second.first, old = p.second.second;
      if (number[g] || taken.count (old)) continue;
      number[g] = old;
      taken.insert (old);
   }
   std::vector<int> fresh;
   for (size_t c = 0; c < sizes.size (); c++)
      if (sizes[c] >= 2 && !number[c]) fresh.push_back ((int) c);
   std::sort (fresh.begin (), fresh.end (),
              [&] (int a, int b) { return sizes[a] > sizes[b]; });
   int next = max_old;
   for (int c : fresh) number[c] = ++next;
   int n_groups = 0;
   for (size_t c = 0; c < sizes.size (); c++)
      if (number[c]) n_groups++;

   st = d.prep ("UPDATE faces SET guess = ?, cluster = ? WHERE id = ?");
   d.exec ("BEGIN");
   for (size_t i = 0; i < rows.size (); i++) {
      if (rows[i].person || rows[i].ignored) {
         sqlite3_bind_null (st, 1);
         sqlite3_bind_null (st, 2);
      } else {
         if (guess[i]) sqlite3_bind_int64 (st, 1, guess[i]);
         else sqlite3_bind_null (st, 1);
         if (group[i] >= 0 && number[group[i]]) sqlite3_bind_int (st, 2, number[group[i]]);
         else sqlite3_bind_null (st, 2);
      }
      sqlite3_bind_int64 (st, 3, rows[i].id);
      sqlite3_step (st);
      sqlite3_reset (st);
   }
   d.exec ("COMMIT");
   sqlite3_finalize (st);
   return n_groups;
}


/******************************************************************************
 *  --add-face: a face a person marked
 ******************************************************************************/

/* the face is enlarged to about this for the detector (a small face) */
#define ADD_FACE_SIZE  200
#define ADD_SCORE_MIN  0.5f    /* a person said it is a face */

static int
run_add_face (const std::string &dbfile, const std::string &models, const std::string &path,
              int x, int y, int w, int h)
{
   Db d;
   if (!open_db (d, dbfile.c_str ())) return 1;

   size_t slash = path.rfind ('/');
   if (slash == std::string::npos) { say ("error not a full path"); return 1; }
   std::string dir = slash ? path.substr (0, slash) : "/", name = path.substr (slash + 1);
   long long file_id = 0;
   sqlite3_stmt *st = d.prep ("SELECT f.id FROM files f JOIN dirs d ON d.id = f.dir_id "
                              "WHERE d.path = ? AND f.name = ?");
   sqlite3_bind_text (st, 1, dir.c_str (), -1, SQLITE_TRANSIENT);
   sqlite3_bind_text (st, 2, name.c_str (), -1, SQLITE_TRANSIENT);
   if (sqlite3_step (st) == SQLITE_ROW) file_id = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   if (!file_id) { say ("error the image is not in the index"); return 1; }

   /* one there already */
   Face want;
   want.x = x; want.y = y; want.w = w; want.h = h;
   st = d.prep ("SELECT id, x, y, w, h FROM faces WHERE file_id = ?");
   sqlite3_bind_int64 (st, 1, file_id);
   while (sqlite3_step (st) == SQLITE_ROW) {
      Face a;
      a.x = sqlite3_column_int (st, 1);
      a.y = sqlite3_column_int (st, 2);
      a.w = sqlite3_column_int (st, 3);
      a.h = sqlite3_column_int (st, 4);
      if (overlap (a, want) > SAME_FACE) {
         say ("exists %lld", (long long) sqlite3_column_int64 (st, 0));
         sqlite3_finalize (st);
         return 0;
      }
   }
   sqlite3_finalize (st);

   std::string detm = models + "/" DETECT_MODEL, recm = models + "/" RECOGNIZE_MODEL;
   if (access (detm.c_str (), R_OK) || access (recm.c_str (), R_OK)) {
      say ("no-models %s", models.c_str ());
      return 1;
   }
   cv::Ptr<cv::FaceDetectorYN> det;
   cv::Ptr<cv::FaceRecognizerSF> rec;
   cv::Mat img;
   double scale;
   Face out;
   bool found_again = false;
   try {
      det = cv::FaceDetectorYN::create (detm, "", cv::Size (320, 320), ADD_SCORE_MIN, 0.3f, 5000);
      rec = cv::FaceRecognizerSF::create (recm, "");
      if (!load_image (path, img, scale)) { say ("error the image cannot be read"); return 1; }

      /* in the pixels of img */
      double fx = x / scale, fy = y / scale, fw = w / scale, fh = h / scale;
      double side = std::max (fw, fh);
      if (side < 4) { say ("error too small"); return 1; }
      double cx = fx + fw / 2, cy = fy + fh / 2, half = side * 1.1;
      int x0 = std::clamp ((int) (cx - half), 0, img.cols - 1);
      int y0 = std::clamp ((int) (cy - half), 0, img.rows - 1);
      int x1 = std::clamp ((int) (cx + half), x0 + 1, img.cols);
      int y1 = std::clamp ((int) (cy + half), y0 + 1, img.rows);
      cv::Rect reg (x0, y0, x1 - x0, y1 - y0);
      double f = std::min ((double) ADD_FACE_SIZE / side,
                           (double) DETECT_SIZE / std::max (reg.width, reg.height));
      cv::Mat crop, found;
      cv::resize (img (reg), crop, cv::Size (), f, f, f > 1 ? cv::INTER_CUBIC : cv::INTER_AREA);
      cv::Mat padded = pad32 (crop);
      det->setInputSize (padded.size ());
      det->detect (padded, found);

      /* the face found most over the one marked */
      Face u;
      u.x = (int) ((fx - x0) * f); u.y = (int) ((fy - y0) * f);
      u.w = (int) (fw * f);        u.h = (int) (fh * f);
      int best = -1;
      double best_ov = 0.3;
      for (int i = 0; i < found.rows; i++) {
         const float *r = found.ptr<float> (i);
         Face g;
         g.x = (int) r[0]; g.y = (int) r[1]; g.w = (int) r[2]; g.h = (int) r[3];
         double ov = overlap (u, g);
         if (ov > best_ov) { best_ov = ov; best = i; }
      }
      cv::Mat feat;
      if (best >= 0) {
         const float *r = found.ptr<float> (best);
         cv::Mat aligned;
         rec->alignCrop (padded, found.row (best), aligned);
         rec->feature (aligned, feat);
         out.x = (int) std::lround ((x0 + std::max (0.0f, r[0]) / f) * scale);
         out.y = (int) std::lround ((y0 + std::max (0.0f, r[1]) / f) * scale);
         out.w = (int) std::lround (r[2] / f * scale);
         out.h = (int) std::lround (r[3] / f * scale);
         out.score = r[14];
         found_again = true;
      } else {
         /* as marked: a square on it, as the recognizer takes a face */
         int s = std::max (8, (int) side);
         int sx = std::clamp ((int) (cx - s / 2.0), 0, std::max (0, img.cols - s));
         int sy = std::clamp ((int) (cy - s / 2.0), 0, std::max (0, img.rows - s));
         s = std::min (s, std::min (img.cols - sx, img.rows - sy));
         cv::Mat sq;
         cv::resize (img (cv::Rect (sx, sy, s, s)), sq, cv::Size (112, 112), 0, 0,
                     s > 112 ? cv::INTER_AREA : cv::INTER_CUBIC);
         rec->feature (sq, feat);
         out = want;
         out.score = 0;
      }
      if (feat.total () != FEATURE_LEN) { say ("error no feature"); return 1; }
      out.feature.assign ((const float *) feat.data, (const float *) feat.data + FEATURE_LEN);
      normalize (out.feature);
      make_thumb (img, scale, out);
   } catch (const cv::Exception &e) {
      /* OpenCV's messages are long: the first line */
      std::string m = e.what ();
      say ("error %s", m.substr (0, m.find ('\n')).c_str ());
      return 1;
   }

   long long face_id = 0;
   if (sqlite3_exec (d.db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
      say ("error the index is busy");
      return 1;
   }
   st = d.prep ("INSERT INTO faces (file_id, x, y, w, h, score, feature) VALUES (?, ?, ?, ?, ?, ?, ?)");
   sqlite3_bind_int64 (st, 1, file_id);
   sqlite3_bind_int (st, 2, out.x);
   sqlite3_bind_int (st, 3, out.y);
   sqlite3_bind_int (st, 4, out.w);
   sqlite3_bind_int (st, 5, out.h);
   sqlite3_bind_double (st, 6, out.score);
   sqlite3_bind_blob (st, 7, out.feature.data (), FEATURE_LEN * sizeof (float), SQLITE_STATIC);
   if (sqlite3_step (st) == SQLITE_DONE) face_id = sqlite3_last_insert_rowid (d.db);
   sqlite3_finalize (st);
   if (face_id) {
      st = d.prep ("INSERT OR REPLACE INTO face_thumbs (face_id, jpeg) VALUES (?, ?)");
      put_thumb (st, face_id, out.thumb);
      sqlite3_finalize (st);
      st = d.prep ("INSERT OR IGNORE INTO face_manual (face_id) VALUES (?)");
      sqlite3_bind_int64 (st, 1, face_id);
      sqlite3_step (st);
      sqlite3_finalize (st);
      st = d.prep ("UPDATE face_scans SET n = n + 1 WHERE file_id = ? AND n >= 0");
      sqlite3_bind_int64 (st, 1, file_id);
      sqlite3_step (st);
      sqlite3_finalize (st);
   }
   d.exec ("COMMIT");
   if (!face_id) { say ("error %s", sqlite3_errmsg (d.db)); return 1; }
   say ("added %lld %d", face_id, found_again ? 1 : 0);
   return 0;
}


/******************************************************************************
 *  --evaluate: the ways of grouping compared on the faces named
 *
 *  The faces a person has named are grouped as if they had no name, and
 *  the names tell how right the groups are: faces of another person in a
 *  group (mixed), and one person's faces spread over many groups (split).
 ******************************************************************************/

typedef std::vector<const float *> Feats;

/* means of the groups (normalized), from the faces of each */
static void
group_means (const Feats &f, const std::vector<int> &grp, int n_groups,
             std::vector<std::vector<float>> &means)
{
   means.assign (n_groups, std::vector<float> (FEATURE_LEN, 0.0f));
   for (size_t i = 0; i < f.size (); i++)
      if (grp[i] >= 0)
         for (int j = 0; j < FEATURE_LEN; j++) means[grp[i]][j] += f[i][j];
   for (auto &m : means) normalize (m);
}


/* the groups numbered 0.. in order, the empty ones gone */
static int
renumber (std::vector<int> &grp)
{
   std::map<int, int> to;
   for (int &g : grp) {
      if (g < 0) continue;
      auto it = to.find (g);
      if (it == to.end ()) it = to.insert ({ g, (int) to.size () }).first;
      g = it->second;
   }
   return (int) to.size ();
}


/* as make_groups: the clearest faces first, each into the group whose mean
 * it is most like (at least "th"; "grow": more for larger groups), else a
 * new group */
static int
leader (const Feats &f, const std::vector<float> &score, const std::vector<size_t> &idx,
        float th, float grow, std::vector<int> &grp, int first)
{
   std::vector<size_t> order (idx);
   std::sort (order.begin (), order.end (),
              [&] (size_t a, size_t b) { return score[a] > score[b]; });
   std::vector<std::vector<float>> sums, means;
   std::vector<int> sizes;

   for (size_t i : order) {
      int g = -1;
      float best = -2;
      for (size_t c = 0; c < means.size (); c++) {
         float need = th + grow * std::log2 ((float) sizes[c]);
         float s = dot (f[i], means[c].data ());
         if (s >= need && s > best) { best = s; g = (int) c; }
      }
      if (g < 0) {
         g = (int) means.size ();
         sums.emplace_back (FEATURE_LEN, 0.0f);
         means.emplace_back ();
         sizes.push_back (0);
      }
      for (int j = 0; j < FEATURE_LEN; j++) sums[g][j] += f[i][j];
      means[g] = sums[g];
      normalize (means[g]);
      sizes[g]++;
      grp[i] = first + g;
   }
   return (int) means.size ();
}


/* each face again into the group whose mean it is most like (at least
 * "th", else alone), the means made again, till nothing moves */
static void
refine (const Feats &f, std::vector<int> &grp, float th, int rounds)
{
   for (int r = 0; r < rounds; r++) {
      int n = renumber (grp);
      std::vector<std::vector<float>> means;
      std::vector<int> sizes (n, 0);
      group_means (f, grp, n, means);
      for (int g : grp) if (g >= 0) sizes[g]++;
      bool moved = false;
      int next = n;
      for (size_t i = 0; i < f.size (); i++) {
         int g = -1;
         float best = th;
         for (int c = 0; c < n; c++) {
            /* a face alone is not drawn to its own mean */
            if (sizes[c] < 2 && c == grp[i]) continue;
            float s = dot (f[i], means[c].data ());
            if (s >= best) { best = s; g = c; }
         }
         if (g < 0) g = (grp[i] >= 0 && sizes[grp[i]] < 2) ? grp[i] : next++;
         if (g != grp[i]) { grp[i] = g; moved = true; }
      }
      if (!moved) break;
   }
   renumber (grp);
}


/* a group whose faces are far apart grouped again within, stricter */
static void
split_wide (const Feats &f, const std::vector<float> &score, std::vector<int> &grp,
            float th, float strict, int min_size)
{
   int n = renumber (grp);
   std::vector<std::vector<size_t>> members (n);
   for (size_t i = 0; i < grp.size (); i++) members[grp[i]].push_back (i);
   int next = n;
   for (int c = 0; c < n; c++) {
      if ((int) members[c].size () < min_size) continue;
      std::vector<int> sub (grp.size (), -1);
      int k = leader (f, score, members[c], strict, 0, sub, 0);
      if (k < 2) continue;
      /* only parts of a few faces each: a person, not poses of one */
      std::vector<int> cnt (k, 0);
      for (size_t i : members[c]) cnt[sub[i]]++;
      int big = 0;
      for (int x : cnt) if (x >= 3) big++;
      if (big < 2) continue;
      for (size_t i : members[c]) grp[i] = sub[i] == 0 ? c : next + sub[i] - 1;
      next += k - 1;
   }
   refine (f, grp, th, 5);
}


struct EvalResult {
   int groups = 0, alone = 0, in_groups = 0;
   int wrong = 0;         /* faces of another person than most of the group */
   int mixed = 0;         /* groups with such faces */
   int mixed_big = 0;     /* of 10 or more faces, 10 % or more of another */
   int outside = 0;       /* faces of a person not in the person's largest group */
   double ms = 0;
};


static EvalResult
evaluate_groups (const std::vector<long long> &person, const std::vector<int> &grp_in,
                 const std::map<long long, std::string> &names, bool show, const char *title,
                 const std::vector<int> *side = nullptr, const std::vector<float> *score = nullptr)
{
   EvalResult r;
   std::vector<int> grp (grp_in);
   int n = renumber (grp);
   std::vector<std::map<long long, int>> count (n);
   std::vector<int> size (n, 0);
   for (size_t i = 0; i < grp.size (); i++) { count[grp[i]][person[i]]++; size[grp[i]]++; }

   std::map<long long, std::map<int, int>> by_person;   /* person -> group -> faces */
   std::vector<std::pair<int, int>> worst;               /* wrong, group */
   for (int c = 0; c < n; c++) {
      if (size[c] < 2) { r.alone++; continue; }
      r.groups++;
      r.in_groups += size[c];
      int top = 0;
      for (auto &p : count[c]) top = std::max (top, p.second);
      int w = size[c] - top;
      r.wrong += w;
      if (w) {
         r.mixed++;
         worst.push_back ({ w, c });
      }
      if (size[c] >= 10 && w * 10 >= size[c]) r.mixed_big++;
   }
   for (size_t i = 0; i < grp.size (); i++) by_person[person[i]][grp[i]]++;
   for (auto &bp : by_person) {
      int all = 0, top = 0;
      for (auto &g : bp.second) { all += g.second; top = std::max (top, g.second); }
      r.outside += all - top;
   }

   if (show && !worst.empty ()) {
      std::sort (worst.rbegin (), worst.rend ());
      printf ("\n%s: the most mixed groups\n", title);
      for (size_t k = 0; k < worst.size () && k < 8; k++) {
         int c = worst[k].second;
         std::vector<std::pair<int, long long>> ps;
         for (auto &p : count[c]) ps.push_back ({ p.second, p.first });
         std::sort (ps.rbegin (), ps.rend ());
         printf ("  %d faces:", size[c]);
         for (size_t j = 0; j < ps.size () && j < 5; j++) {
            auto it = names.find (ps[j].second);
            printf (" %s %d", it != names.end () ? it->second.c_str () : "?", ps[j].first);
         }
         if (ps.size () > 5) printf (" ...");
         if (side && score) {
            /* small, unclear faces look alike to the recognizer */
            double sz = 0, sc = 0;
            int m = 0;
            for (size_t i = 0; i < grp.size (); i++)
               if (grp[i] == c) { sz += (*side)[i]; sc += (*score)[i]; m++; }
            if (m) printf ("  [face %.0f px, score %.2f]", sz / m, sc / m);
         }
         printf ("\n");
      }
   }
   return r;
}


static int
run_evaluate (const std::string &dbfile)
{
   Db d;
   if (sqlite3_open_v2 (dbfile.c_str (), &d.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      fprintf (stderr, "cannot open %s\n", dbfile.c_str ());
      return 1;
   }
   sqlite3_busy_handler (d.db, busy_poll, nullptr);

   std::vector<std::vector<float>> store;
   std::vector<float> score;
   std::vector<long long> person;
   std::map<long long, std::string> names;
   std::vector<int> side;   /* the shorter side of the face in the image */
   sqlite3_stmt *st = d.prep ("SELECT fa.feature, fa.score, p.id, p.name, min (fa.w, fa.h) "
                              "FROM faces fa JOIN persons p ON p.id = fa.person_id "
                              "WHERE fa.ignored = 0");
   while (st && sqlite3_step (st) == SQLITE_ROW) {
      if (sqlite3_column_bytes (st, 0) != FEATURE_LEN * (int) sizeof (float)) continue;
      const float *b = (const float *) sqlite3_column_blob (st, 0);
      store.emplace_back (b, b + FEATURE_LEN);
      score.push_back ((float) sqlite3_column_double (st, 1));
      person.push_back (sqlite3_column_int64 (st, 2));
      names[person.back ()] = (const char *) sqlite3_column_text (st, 3);
      side.push_back (sqlite3_column_int (st, 4));
   }
   sqlite3_finalize (st);
   if (store.empty ()) {
      printf ("no faces named: nothing to compare with\n");
      return 0;
   }
   Feats f;
   for (auto &v : store) f.push_back (v.data ());
   std::vector<size_t> all (f.size ());
   for (size_t i = 0; i < all.size (); i++) all[i] = i;

   printf ("%zu faces named, %zu people: grouped as if they had no name\n",
           f.size (), names.size ());
   printf ("(threshold now %.2f; the guesses of the people named are not part of this)\n\n",
           GROUP_SIM);

   struct Way { const char *title; int kind; float th; int min_side; float min_score; };
   const Way ways[] = {
      { "A before: leader 0.42",                  0, 0.42f, 0, 0 },
      { "B A + faces moved to the nearest group", 1, 0.42f, 0, 0 },
      { "C B + wide groups split",                2, 0.42f, 0, 0 },
      { "D larger groups stricter + B",           3, 0.42f, 0, 0 },
      { "E B, threshold 0.46 (now)",              1, 0.46f, 0, 0 },
      { "F B, threshold 0.50",                    1, 0.50f, 0, 0 },
      { "G C, faces under 64 px alone",           2, 0.42f, 64, 0 },
      { "H C, faces under 80 px or score<0.9 alone", 2, 0.42f, 80, 0.90f },
   };
   std::vector<std::pair<std::string, EvalResult>> results;
   std::vector<std::vector<int>> grps;
   for (const Way &w : ways) {
      std::vector<int> grp (f.size (), -1);
      long long t0 = now_usec ();
      /* the faces left out: each alone (not in a group) */
      std::vector<size_t> in;
      int next = (int) f.size ();
      for (size_t i : all) {
         if ((w.min_side && side[i] < w.min_side) || (w.min_score > 0 && score[i] < w.min_score))
            grp[i] = next++;
         else
            in.push_back (i);
      }
      Feats fin;
      std::vector<float> sin;
      std::vector<size_t> idx;
      for (size_t k = 0; k < in.size (); k++) {
         fin.push_back (f[in[k]]);
         sin.push_back (score[in[k]]);
         idx.push_back (k);
      }
      std::vector<int> g2 (fin.size (), -1);
      leader (fin, sin, idx, w.th, w.kind == 3 ? 0.02f : 0.0f, g2, 0);
      if (w.kind >= 1) refine (fin, g2, w.th, 10);
      if (w.kind == 2) split_wide (fin, sin, g2, w.th, w.th + 0.08f, 6);
      for (size_t k = 0; k < in.size (); k++) grp[in[k]] = g2[k];
      EvalResult r = evaluate_groups (person, grp, names, false, w.title);
      r.ms = (now_usec () - t0) / 1000.0;
      results.push_back ({ w.title, r });
      grps.push_back (grp);
      fprintf (stderr, "%s: done\n", w.title);
   }

   printf ("%-42s %7s %6s %7s %6s %9s %8s %8s\n", "way", "groups", "alone", "wrong",
           "mixed", "mixed>=10", "outside", "seconds");
   for (auto &x : results)
      printf ("%-42s %7d %6d %7d %6d %9d %8d %8.1f\n", x.first.c_str (), x.second.groups,
              x.second.alone, x.second.wrong, x.second.mixed, x.second.mixed_big,
              x.second.outside, x.second.ms / 1000.0);
   printf ("\n  groups: of 2 faces or more; alone: faces in no group\n"
           "  wrong: faces of another person than most of their group (fewer is better)\n"
           "  mixed: groups with such faces; mixed>=10: of 10 faces or more, 10 %% or more\n"
           "  outside: faces not in the largest group of their person (one person split\n"
           "           over groups; fewer is better, but some is fine: they are named\n"
           "           one group at a time)\n");
   for (size_t k = 0; k < results.size (); k++)
      evaluate_groups (person, grps[k], names, true, results[k].first.c_str (), &side, &score);

   /* the guesses: each face against the mean of each person (its own
    * person's without it), as gimv-faces guesses the faces not named */
   {
      std::map<long long, std::vector<float>> sums;
      std::map<long long, int> cnt;
      for (size_t i = 0; i < f.size (); i++) {
         auto &m = sums[person[i]];
         if (m.empty ()) m.assign (FEATURE_LEN, 0.0f);
         for (int j = 0; j < FEATURE_LEN; j++) m[j] += f[i][j];
         cnt[person[i]]++;
      }
      std::vector<std::pair<long long, std::vector<float>>> means;
      for (auto &x : sums) {
         std::vector<float> m = x.second;
         normalize (m);
         means.push_back ({ x.first, m });
      }
      const float ths[] = { 0.40f, GUESS_SIM, 0.50f, 0.55f };
      int guessed[4] = { 0 }, right[4] = { 0 }, n = 0;
      int small_n = 0, small_right = 0;
      for (size_t i = 0; i < f.size (); i++) {
         if (cnt[person[i]] < 3) continue;   /* the person known from other faces */
         n++;
         float best = -2;
         long long who = 0;
         for (auto &m : means) {
            float s;
            if (m.first == person[i]) {
               /* the mean without this face */
               std::vector<float> o = sums[person[i]];
               for (int j = 0; j < FEATURE_LEN; j++) o[j] -= f[i][j];
               normalize (o);
               s = dot (f[i], o.data ());
            } else {
               s = dot (f[i], m.second.data ());
            }
            if (s > best) { best = s; who = m.first; }
         }
         for (int t = 0; t < 4; t++)
            if (best >= ths[t]) {
               guessed[t]++;
               if (who == person[i]) right[t]++;
            }
         if (side[i] < 80 && best >= GUESS_SIM) {
            small_n++;
            if (who == person[i]) small_right++;
         }
      }
      printf ("\nguesses (each face named against the faces of each person, %d faces of\n"
              "people with 3 faces or more):\n", n);
      for (int t = 0; t < 4; t++)
         printf ("  at %.2f: %5.1f %% of the faces get a guess, %5.1f %% of the guesses right%s\n",
                 ths[t], n ? 100.0 * guessed[t] / n : 0.0,
                 guessed[t] ? 100.0 * right[t] / guessed[t] : 0.0,
                 ths[t] == GUESS_SIM ? "  (now)" : "");
      printf ("  faces under 80 px at %.2f: %d guesses, %.1f %% right\n", GUESS_SIM, small_n,
              small_n ? 100.0 * small_right / small_n : 0.0);
   }
   return 0;
}


/******************************************************************************
 *  --export: the faces of each group as small images, to look at
 ******************************************************************************/

static void
export_groups (Db &d, const std::string &out)
{
   sqlite3_stmt *st = d.prep (
      "SELECT fa.id, fa.cluster, fa.x, fa.y, fa.w, fa.h, d.path, f.name, "
      "  (SELECT count(*) FROM faces c WHERE c.cluster = fa.cluster) "
      "FROM faces fa JOIN files f ON f.id = fa.file_id JOIN dirs d ON d.id = f.dir_id "
      "WHERE fa.cluster IS NOT NULL "
      "ORDER BY fa.cluster, fa.score DESC");
   std::map<int, int> written;
   std::string last_path;
   cv::Mat img;

   mkdir (out.c_str (), 0755);
   while (st && sqlite3_step (st) == SQLITE_ROW && !quit) {
      int cluster = sqlite3_column_int (st, 1);
      if (written[cluster] >= 40) continue;

      std::string path ((const char *) sqlite3_column_text (st, 6));
      if (path.back () != '/') path += '/';
      path += (const char *) sqlite3_column_text (st, 7);
      if (path != last_path) {
         img = cv::imread (path, cv::IMREAD_COLOR);
         last_path = path;
      }
      if (img.empty ()) continue;

      int x = sqlite3_column_int (st, 2), y = sqlite3_column_int (st, 3);
      int w = sqlite3_column_int (st, 4), h = sqlite3_column_int (st, 5);
      int m = std::max (w, h) / 4;
      cv::Rect r = cv::Rect (x - m, y - m, w + 2 * m, h + 2 * m)
                   & cv::Rect (0, 0, img.cols, img.rows);
      if (r.width <= 0 || r.height <= 0) continue;
      cv::Mat crop;
      cv::resize (img (r), crop, cv::Size (160, (int) (160.0 * r.height / r.width)));

      char dir[64], name[64];
      snprintf (dir, sizeof (dir), "/group-%03d-%d", cluster, sqlite3_column_int (st, 8));
      std::string gdir = out + dir;
      mkdir (gdir.c_str (), 0755);
      snprintf (name, sizeof (name), "/%lld.jpg", (long long) sqlite3_column_int64 (st, 0));
      cv::imwrite (gdir + name, crop);
      written[cluster]++;
      check_stdin ();
   }
   sqlite3_finalize (st);
}


/******************************************************************************
 *  main
 ******************************************************************************/

static void
usage (void)
{
   fprintf (stderr, "usage: gimv-faces --db FILE --models DIR [--regroup] [--export DIR] [--] FOLDER...\n"
                    "       gimv-faces --evaluate --db FILE\n"
                    "       gimv-faces --group-only --db FILE\n"
                    "       gimv-faces --add-face X,Y,W,H --db FILE --models DIR IMAGE\n");
}


int
main (int argc, char *argv[])
{
   std::string dbfile, models, export_dir;
   std::vector<std::string> dirs;
   bool opts = true, regroup = false, evaluate = false, group_only = false;
   int ax = 0, ay = 0, aw = 0, ah = 0;
   bool add_face = false;

   signal (SIGPIPE, SIG_IGN);

   for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (opts && a == "--") { opts = false; continue; }
      if (opts && a == "--db" && i + 1 < argc) { dbfile = argv[++i]; continue; }
      if (opts && a == "--models" && i + 1 < argc) { models = argv[++i]; continue; }
      if (opts && a == "--regroup") { regroup = true; continue; }
      if (opts && a == "--evaluate") { evaluate = true; continue; }
      if (opts && a == "--group-only") { group_only = true; continue; }
      if (opts && a == "--add-face" && i + 1 < argc) {
         add_face = sscanf (argv[++i], "%d,%d,%d,%d", &ax, &ay, &aw, &ah) == 4;
         if (!add_face) { usage (); return 2; }
         continue;
      }
      if (opts && a == "--export" && i + 1 < argc) { export_dir = argv[++i]; continue; }
      if (opts && a.size () > 1 && a[0] == '-') { usage (); return 2; }
      while (a.size () > 1 && a.back () == '/') a.pop_back ();
      if (!a.empty ()) dirs.push_back (a);
   }
   if (evaluate && !dbfile.empty ())
      return run_evaluate (dbfile);
   if (add_face) {
      if (dbfile.empty () || models.empty () || dirs.size () != 1) { usage (); return 2; }
      /* the image as given (the trailing slashes taken off above do not
       * matter for a file) */
      return run_add_face (dbfile, models, dirs[0], ax, ay, aw, ah);
   }
   if (group_only && !dbfile.empty ()) {
      std::string lockfile = dbfile + ".faces-lock";
      int lockfd = open (lockfile.c_str (), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
      if (lockfd < 0 || flock (lockfd, LOCK_EX | LOCK_NB) < 0) {
         say ("busy");
         return 0;
      }
      Db d;
      if (!open_db (d, dbfile.c_str ())) return 1;
      int groups = make_groups (d);
      say ("done 0 0 %d", groups);
      return 0;
   }
   if (dbfile.empty () || models.empty () || dirs.empty ()) {
      usage ();
      return 2;
   }

   lower_priority ();
   cv::setNumThreads (2);

   /* one at a time */
   std::string lockfile = dbfile + ".faces-lock";
   int lockfd = open (lockfile.c_str (), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
   if (lockfd < 0 || flock (lockfd, LOCK_EX | LOCK_NB) < 0) {
      say ("busy");
      return 0;
   }

   Db d;
   if (!open_db (d, dbfile.c_str ())) return 1;

   if (!export_dir.empty ()) {
      export_groups (d, export_dir);
      say ("done 0 0 0");
      return 0;
   }

   std::string detm = models + "/" DETECT_MODEL, recm = models + "/" RECOGNIZE_MODEL;
   if (access (detm.c_str (), R_OK) || access (recm.c_str (), R_OK)) {
      say ("no-models %s", models.c_str ());
      return 0;
   }

   cv::Ptr<cv::FaceDetectorYN> det;
   cv::Ptr<cv::FaceRecognizerSF> rec;
   try {
      det = cv::FaceDetectorYN::create (detm, "", cv::Size (320, 320), SCORE_MIN, 0.3f, 5000);
      rec = cv::FaceRecognizerSF::create (recm, "");
   } catch (const cv::Exception &e) {
      say ("error the models cannot be read: %s", e.what ());
      return 1;
   }

   guard_open (d, dbfile);

   std::vector<Todo> todo;
   long long total = 0;
   list_files (d, dirs, todo, total);
   std::vector<std::vector<Face>> missing;
   list_missing_thumbs (d, todo, missing);
   say ("start %zu %lld", todo.size (), total);

   long long last_commit = now_usec (), last_say = 0;
   size_t done = 0, n_faces = 0;
   bool changed = false, failed = false;
   std::vector<Result> pending;
   sqlite3_stmt *faces_of = d.prep ("SELECT x, y, w, h FROM faces WHERE file_id = ?");

   for (const Todo &t : todo) {
      Result r;

      check_stdin ();
      if (quit) break;

      if (t.missing) {
         cv::Mat img;
         double scale;
         guard_set (t.id);
         r.ok = load_image (t.path, img, scale) ? 1 : 0;
         guard_set (0);
         r.faces = *t.missing;
         if (r.ok)
            for (Face &f : r.faces) make_thumb (img, scale, f);
         r.todo = &t;
         pending.push_back (std::move (r));
         done++;
         long long now = now_usec ();
         if (now - last_commit >= COMMIT_USEC) {
            write_results (d, pending);
            last_commit = now;
         }
         if (now - last_say >= 1000000LL || done == todo.size ()) {
            say ("progress %zu %zu", done, todo.size ());
            last_say = now;
         }
         continue;
      }

      std::vector<Face> known;
      if (t.closeup) {
         sqlite3_bind_int64 (faces_of, 1, t.id);
         while (sqlite3_step (faces_of) == SQLITE_ROW) {
            Face f;
            f.x = sqlite3_column_int (faces_of, 0);
            f.y = sqlite3_column_int (faces_of, 1);
            f.w = sqlite3_column_int (faces_of, 2);
            f.h = sqlite3_column_int (faces_of, 3);
            known.push_back (std::move (f));
         }
         sqlite3_reset (faces_of);
      }
      guard_set (t.id);
      r.ok = read_faces (t.path, det, rec, t.closeup, known, r.faces);
      guard_set (0);
      if (r.ok < 0) {
         /* not this image's fault: stop, without marking it */
         quit = true;
         failed = true;
         break;
      }
      r.todo = &t;
      n_faces += r.faces.size ();
      /* the groups again: only for new faces after a second look alone */
      if (!t.closeup || !r.faces.empty ()) changed = true;
      pending.push_back (std::move (r));
      done++;

      /* written in short transactions, not to keep the indexer waiting
       * while an image is read */
      long long now = now_usec ();
      if (now - last_commit >= COMMIT_USEC) {
         write_results (d, pending);
         last_commit = now;
      }
      if (now - last_say >= 1000000LL || done == todo.size ()) {
         say ("progress %zu %zu", done, todo.size ());
         last_say = now;
      }
   }
   write_results (d, pending);
   sqlite3_finalize (faces_of);
   guard_close (dbfile);

   /* the groups again when faces came or went, or when asked (names given) */
   int groups = -1;
   if (!quit && (changed || regroup))
      groups = make_groups (d);
   if (groups < 0) {
      sqlite3_stmt *st = d.prep ("SELECT count(DISTINCT cluster) FROM faces");
      groups = st && sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int (st, 0) : 0;
      sqlite3_finalize (st);
   }
   if (failed) return 1;
   if (!quit)
      say ("done %zu %zu %d", done, n_faces, groups);
   return 0;
}
