/***************************************************************************
 * datafilter.c - miniSEED data filtering and organization.
 *
 * Opens one or more user specified files and outputs each record that
 * matches the selected criteria.  Optionally, records may be trimmed
 * (at record or sample level) to selected time ranges.
 *
 * In general critical error messages are prefixed with "ERROR:" and
 * the return code will be 1.  On successful operation the return
 * code will be 0.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 ***************************************************************************/

/* _ISOC9X_SOURCE needed to get a declaration for llabs on some archs */
#define _ISOC9X_SOURCE

#define __STDC_FORMAT_MACROS
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <libmseed.h>

#include "dsarchive.h"

#define VERSION "0.1"
#define PACKAGE "datafilter"

/* Input/output file selection information containers */
typedef struct Filelink_s
{
  char *filename; /* Input file name */
  uint64_t startoffset; /* Byte offset to start reading, 0 = unused */
  uint64_t endoffset; /* Byte offset to end reading, 0 = unused */
  struct Filelink_s *next;
} Filelink;

/* Archive output structure definition containers */
typedef struct Archive_s
{
  DataStream datastream;
  struct Archive_s *next;
} Archive;

static int readfile (Filelink *flp);
static int trimrecord (MSRecord *msr, hptime_t recendtime,
                       hptime_t newstart, hptime_t newend,
                       Filelink *flp, int64_t fpos);
static void writerecord (char *record, int reclen, void *handlerdata);
static int findselectlimits (Selections *select, char *srcname,
                             hptime_t starttime, hptime_t endtime,
                             hptime_t *selectstart, hptime_t *selectend);
static void printwritten (MSTraceList *mstl);
static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static int setofilelimit (int limit);
static int addfile (char *filename);
static int addlistfile (char *filename);
static int addarchive (const char *path, const char *layout);
static int readregexfile (char *regexfile, char **pppattern);
static void usage (int level);

static flag verbose = 0;
static char prunedata = 'r'; /* Prune data: 'r= record level, 's' = sample level */
static int reclen = -1; /* Input data record length, autodetected in most cases */
static hptime_t starttime = HPTERROR; /* Limit to records containing or after starttime */
static hptime_t endtime = HPTERROR; /* Limit to records containing or before endtime */

static regex_t *match = 0; /* Compiled match regex */
static regex_t *reject = 0; /* Compiled reject regex */

static char *outputfile = 0; /* Single output file */
static flag outputmode = 0; /* Mode for single output file: 0=overwrite, 1=append */
static Archive *archiveroot = 0; /* Output file structures */

static Filelink *filelist = 0; /* List of input files */
static Filelink *filelisttail = 0; /* Tail of list of input files */
static Selections *selections = 0; /* List of data selections */

static char *writtenfile = 0; /* File to write summary of output records */
static char *writtenprefix = 0; /* Prefix for summary of output records */
static MSTraceList *writtentl = 0; /* TraceList of output records */

static uint64_t totalrecsout = 0;
static uint64_t totalbytesout = 0;

static FILE *ofp = 0;

int
main (int argc, char **argv)
{
  Filelink *flp;
  Archive *arch;
  char *wb = "wb";
  char *ab = "ab";
  char *mode;
  char *leapsecondfile = NULL;

  /* Set default error message prefix */
  ms_loginit (NULL, NULL, NULL, "ERROR: ");

  /* Process input parameters */
  if (processparam (argc, argv) < 0)
    return 1;

  /* Read leap second list file if env. var. LIBMSEED_LEAPSECOND_FILE is set */
  if ((leapsecondfile = getenv ("LIBMSEED_LEAPSECOND_FILE")))
  {
    if (strcmp (leapsecondfile, "NONE"))
      ms_readleapsecondfile (leapsecondfile);
  }
  else if (verbose >= 1)
  {
    ms_log (1, "Warning: No leap second file specified with LIBMSEED_LEAPSECOND_FILE\n");
    ms_log (1, "  This is highly recommended, see man page for details.\n");
  }

  /* Data stream archiving maximum concurrent open files */
  if (archiveroot)
    ds_maxopenfiles = 50;

  /* Increase open file limit if necessary, in general we need the
   * ds_maxopenfiles and some wiggle room. */
  setofilelimit (ds_maxopenfiles + 20);

  /* Init written MSTraceList */
  if (writtenfile)
    if ((writtentl = mstl_init (writtentl)) == NULL)
      return 1;

  /* Open the output file if specified */
  if (outputfile)
  {
    /* Decide if we are appending or overwriting */
    mode = (totalbytesout || outputmode) ? ab : wb;

    if (strcmp (outputfile, "-") == 0)
    {
      ofp = stdout;
    }
    else if ((ofp = fopen (outputfile, mode)) == NULL)
    {
      ms_log (2, "Cannot open output file: %s (%s)\n",
              outputfile, strerror (errno));
      return 1;
    }
  }

  /* Process each input file in the order they were specified */
  flp = filelist;

  while (flp != 0)
  {
    if (readfile (flp))
      return 1;

    flp = flp->next;
  }

  /* Close output files */
  if (ofp)
  {
    fclose (ofp);
    ofp = 0;
  }

  if (archiveroot)
  {
    arch = archiveroot;
    while (arch)
    {
      ds_streamproc (&arch->datastream, NULL, 0, verbose - 1);
      arch = arch->next;
    }
  }

  if (verbose)
  {
    ms_log (1, "Wrote %" PRIu64 " bytes of %" PRIu64 " records to output file(s)\n",
            totalbytesout, totalrecsout);
  }

  if (writtenfile)
  {
    printwritten (writtentl);
    mstl_free (&writtentl, 1);
  }

  return 0;
} /* End of main() */

/***************************************************************************
 * readfile:
 *
 * Read input file and output records that match selection criteria.
 *
 * Returns 0 on success and -1 otherwise.
 ***************************************************************************/
static int
readfile (Filelink *flp)
{
  MSFileParam *msfp = NULL;
  MSRecord *msr = NULL;
  off_t fpos = 0;

  Selections *matchsp = 0;
  SelectTime *matchstp = 0;

  hptime_t recstarttime = HPTERROR;
  hptime_t recendtime = HPTERROR;
  hptime_t selectstart = HPTERROR;
  hptime_t selectend = HPTERROR;
  hptime_t newstart = HPTERROR;
  hptime_t newend = HPTERROR;
  hptime_t selecttime = HPTERROR;

  char srcname[100] = {0};
  char timestr[32] = {0};
  int retcode;
  int rv;

  if (!flp)
    return -1;

  if (verbose)
  {
    if (flp->startoffset || flp->endoffset)
      ms_log (1, "Reading: %s [range %" PRIu64 ":%" PRIu64 "]\n",
              flp->filename, flp->startoffset, flp->endoffset);
    else
      ms_log (1, "Reading: %s\n", flp->filename);
  }

  /* Instruct libmseed to start at specified offset by setting a negative file position */
  fpos = -flp->startoffset; /* Unset value is a 0, making this a non-operation */

  /* Loop over the input file */
  while ((retcode = ms_readmsr_main (&msfp, &msr, flp->filename, reclen, &fpos, NULL, 1, 0, selections, verbose - 2)) == MS_NOERROR)
  {
    /* Break out as EOF if we have read past end offset */
    if (flp->endoffset > 0 && fpos >= flp->endoffset)
    {
      retcode = MS_ENDOFFILE;
      break;
    }

    recstarttime = msr->starttime;
    recendtime = msr_endtime (msr);

    /* Generate the srcname with the quality code */
    msr_srcname (msr, srcname, 1);

    /* Check if record matches start time criteria: starts after or contains starttime */
    if ((starttime != HPTERROR) && (recstarttime < starttime && !(recstarttime <= starttime && recendtime >= starttime)))
    {
      if (verbose >= 3)
      {
        ms_hptime2seedtimestr (recstarttime, timestr, 1);
        ms_log (1, "Skipping (starttime) %s, %s\n", srcname, timestr);
      }
      continue;
    }

    /* Check if record matches end time criteria: ends after or contains endtime */
    if ((endtime != HPTERROR) && (recendtime > endtime && !(recstarttime <= endtime && recendtime >= endtime)))
    {
      if (verbose >= 3)
      {
        ms_hptime2seedtimestr (recstarttime, timestr, 1);
        ms_log (1, "Skipping (endtime) %s, %s\n", srcname, timestr);
      }
      continue;
    }

    /* Check if record is matched by the match regex */
    if (match)
    {
      if (regexec (match, srcname, 0, 0, 0) != 0)
      {
        if (verbose >= 3)
        {
          ms_hptime2seedtimestr (recstarttime, timestr, 1);
          ms_log (1, "Skipping (match) %s, %s\n", srcname, timestr);
        }
        continue;
      }
    }

    /* Check if record is rejected by the reject regex */
    if (reject)
    {
      if (regexec (reject, srcname, 0, 0, 0) == 0)
      {
        if (verbose >= 3)
        {
          ms_hptime2seedtimestr (recstarttime, timestr, 1);
          ms_log (1, "Skipping (reject) %s, %s\n", srcname, timestr);
        }
        continue;
      }
    }

    /* Check if record is matched by selection */
    if (selections)
    {
      if (!(matchsp = ms_matchselect (selections, srcname, recstarttime, recendtime, &matchstp)))
      {
        if (verbose >= 3)
        {
          ms_hptime2seedtimestr (recstarttime, timestr, 1);
          ms_log (1, "Skipping (selection) %s, %s\n", srcname, timestr);
        }
        continue;
      }
    }

    if (verbose > 2)
      msr_print (msr, verbose - 3);

    /* If record is not completely selected search for joint selection limits */
    if (matchstp && !(matchstp->starttime <= recstarttime && matchstp->endtime >= recendtime))
    {
      if (findselectlimits (matchsp, srcname, recstarttime, recendtime, &selectstart, &selectend))
      {
        ms_log (2, "Problem in findselectlimits(), please report\n");
      }
    }

    newstart = HPTERROR;
    newend = HPTERROR;

    /* If pruning at the sample level trim right at the start/end times */
    if (prunedata == 's')
    {
      /* Determine strictest start time (selection time or global start time) */
      if (starttime != HPTERROR && selectstart != HPTERROR)
        selecttime = (starttime > selectstart) ? starttime : selectstart;
      else if (selectstart != HPTERROR)
        selecttime = selectstart;
      else
        selecttime = starttime;

      /* If the record crosses the start time */
      if (selecttime != HPTERROR && (selecttime > recstarttime) && (selecttime <= recendtime))
      {
        newstart = selecttime;
      }

      /* Determine strictest end time (selection time or global end time) */
      if (endtime != HPTERROR && selectend != HPTERROR)
        selecttime = (endtime < selectend) ? endtime : selectend;
      else if (selectend != HPTERROR)
        selecttime = selectend;
      else
        selecttime = endtime;

      /* If the Record crosses the end time */
      if (selecttime != HPTERROR && (selecttime >= recstarttime) && (selecttime < recendtime))
      {
        newend = selecttime;
      }
    }

    /* Write out the data, either the record needs to be trimmed (and will be
     * send to the record writer) or we send it directly to the record writer. */
    if (newstart != HPTERROR || newend != HPTERROR)
    {
      rv = trimrecord (msr, recendtime, newstart, newend, flp, (int64_t)fpos);

      if (rv == -1)
      {
        continue;
      }
      if (rv == -2)
      {
        ms_log (2, "Cannot unpack miniSEED from byte offset %" PRId64 " in %s\n",
                (int64_t)fpos, flp->filename);
        break;
      }
    }
    else
    {
      writerecord (msr->record, msr->reclen, msr);
    }

    /* Break out as EOF if record is at or beyond end offset */
    if (flp->endoffset > 0 && (fpos + msr->reclen) >= flp->endoffset)
    {
      retcode = MS_ENDOFFILE;
      break;
    }
  } /* End of looping through records in file */

  /* Critical error if file was not read properly */
  if (retcode != MS_ENDOFFILE)
  {
    ms_log (2, "Cannot read %s: %s\n", flp->filename, ms_errorstr (retcode));
    ms_readmsr_main (&msfp, &msr, NULL, 0, NULL, NULL, 0, 0, NULL, 0);
    return -1;
  }

  /* Make sure everything is cleaned up */
  ms_readmsr_main (&msfp, &msr, NULL, 0, NULL, NULL, 0, 0, NULL, 0);

  return 0;
} /* End of readfile() */

/***************************************************************************
 * trimrecord():
 *
 * Unpack a data record and trim samples, either from the beginning or
 * the end, to fit the specified newstart and/or newend times.  The
 * newstart and newend times are treated as arbitrary boundaries, not
 * as explicit new start/end times, this routine calculates which
 * samples fit within the new boundaries.
 *
 * Return 0 on success, -1 on failure or skip and -2 on unpacking errors.
 ***************************************************************************/
static int
trimrecord (MSRecord *msr, hptime_t recendtime,
            hptime_t newstart, hptime_t newend,
            Filelink *flp, int64_t fpos)
{
  MSRecord *datamsr = NULL;
  hptime_t hpdelta;

  char srcname[100] = {0};
  char stime[32] = {0};
  char etime[32] = {0};

  int trimsamples;
  int samplesize;
  int64_t packedsamples;
  int packedrecords;
  int retcode;

  if (!msr)
    return -1;

  srcname[0] = '\0';
  stime[0] = '\0';
  etime[0] = '\0';

  /* Sanity check for new start/end times */
  if ((newstart != HPTERROR && newend != HPTERROR && newstart > newend) ||
      (newstart != HPTERROR && (newstart < msr->starttime || newstart > recendtime)) ||
      (newend != HPTERROR && (newend > recendtime || newend < msr->starttime)))
  {
    ms_log (2, "Problem with new start/end record bound times.\n");
    msr_srcname (msr, srcname, 1);
    ms_log (2, "  Original record %s from %s (byte offset: %" PRId64 ")\n",
            srcname, flp->filename, fpos);
    ms_hptime2seedtimestr (msr->starttime, stime, 1);
    ms_hptime2seedtimestr (recendtime, etime, 1);
    ms_log (2, "       Start: %s       End: %s\n", stime, etime);
    if (newstart == HPTERROR)
      strcpy (stime, "NONE");
    else
      ms_hptime2seedtimestr (newstart, stime, 1);
    if (newend == HPTERROR)
      strcpy (etime, "NONE");
    else
      ms_hptime2seedtimestr (newend, etime, 1);
    ms_log (2, " Start bound: %-24s End bound: %-24s\n", stime, etime);

    return -1;
  }

  /* Check for unsupported data encoding, can only trim what can be packed */
  if (msr->encoding != DE_INT16 && msr->encoding != DE_INT32 &&
      msr->encoding != DE_FLOAT32 && msr->encoding != DE_FLOAT64 &&
      msr->encoding != DE_STEIM1 && msr->encoding != DE_STEIM2)
  {
    if (verbose)
    {
      msr_srcname (msr, srcname, 0);
      ms_hptime2seedtimestr (msr->starttime, stime, 1);
      if (msr->encoding == DE_ASCII)
        ms_log (1, "Skipping trim of %s (%s), ASCII encoded data\n",
                srcname, stime, msr->encoding);
      else
        ms_log (1, "Skipping trim of %s (%s), unsupported encoding (%d: %s)\n",
                srcname, stime, msr->encoding, ms_encodingstr (msr->encoding));
    }

    /* Write whole record to output */
    writerecord (msr->record, msr->reclen, msr);

    return 0;
  }

  /* Unpack data record header including data samples */
  if ((retcode = msr_unpack (msr->record, msr->reclen, &datamsr, 1, verbose - 1)) != MS_NOERROR)
  {
    ms_log (2, "Cannot unpack miniSEED record: %s\n", ms_errorstr (retcode));
    return -2;
  }

  if (verbose > 1)
  {
    msr_srcname (datamsr, srcname, 0);
    ms_log (1, "Triming record: %s (%c)\n", srcname, datamsr->dataquality);
    ms_hptime2seedtimestr (datamsr->starttime, stime, 1);
    ms_hptime2seedtimestr (recendtime, etime, 1);
    ms_log (1, "       Start: %s        End: %s\n", stime, etime);
    if (newstart == HPTERROR)
      strcpy (stime, "NONE");
    else
      ms_hptime2seedtimestr (newstart, stime, 1);
    if (newend == HPTERROR)
      strcpy (etime, "NONE");
    else
      ms_hptime2seedtimestr (newend, etime, 1);
    ms_log (1, " Start bound: %-24s  End bound: %-24s\n", stime, etime);
  }

  /* Determine sample period in high precision time ticks */
  hpdelta = (datamsr->samprate) ? (hptime_t) (HPTMODULUS / datamsr->samprate) : 0;

  /* Remove samples from the beginning of the record */
  if (newstart != HPTERROR && hpdelta)
  {
    hptime_t newstarttime;

    /* Determine new start time and the number of samples to trim */
    trimsamples = 0;
    newstarttime = datamsr->starttime;

    while (newstarttime < newstart && trimsamples < datamsr->samplecnt)
    {
      newstarttime += hpdelta;
      trimsamples++;
    }

    if (trimsamples >= datamsr->samplecnt)
    {
      if (verbose > 1)
        ms_log (1, "All samples would be trimmed from record, skipping\n");

      msr_free (&datamsr);
      return -1;
    }

    if (verbose > 2)
    {
      ms_hptime2seedtimestr (newstarttime, stime, 1);
      ms_log (1, "Removing %d samples from the start, new start time: %s\n", trimsamples, stime);
    }

    samplesize = ms_samplesize (datamsr->sampletype);

    memmove (datamsr->datasamples,
             (char *)datamsr->datasamples + (samplesize * trimsamples),
             samplesize * (datamsr->numsamples - trimsamples));

    datamsr->numsamples -= trimsamples;
    datamsr->samplecnt -= trimsamples;
    datamsr->starttime = newstarttime;
  }

  /* Remove samples from the end of the record */
  if (newend != HPTERROR && hpdelta)
  {
    hptime_t newendtime;

    /* Determine new end time and the number of samples to trim */
    trimsamples = 0;
    newendtime = recendtime;

    while (newendtime > newend && trimsamples < datamsr->samplecnt)
    {
      newendtime -= hpdelta;
      trimsamples++;
    }

    if (trimsamples >= datamsr->samplecnt)
    {
      if (verbose > 1)
        ms_log (1, "All samples would be trimmed from record, skipping\n");

      msr_free (&datamsr);
      return -1;
    }

    if (verbose > 2)
    {
      ms_hptime2seedtimestr (newendtime, etime, 1);
      ms_log (1, "Removing %d samples from the end, new end time: %s\n", trimsamples, etime);
    }

    datamsr->numsamples -= trimsamples;
    datamsr->samplecnt -= trimsamples;
  }

  /* Repacking the record will apply any unapplied time corrections to the start time,
   * make sure the flag is set to indicate that the correction has been applied. */
  if (datamsr->fsdh && datamsr->fsdh->time_correct != 0 && !(datamsr->fsdh->act_flags & 0x02))
  {
    datamsr->fsdh->act_flags |= (1 << 1);
  }

  /* Pack the data record into the global record buffer used by writetraces() */
  packedrecords = msr_pack (datamsr, &writerecord, datamsr,
                            &packedsamples, 1, verbose - 1);

  if (packedrecords != 1)
  {
    msr_srcname (datamsr, srcname, 1);
    ms_hptime2seedtimestr (msr->starttime, stime, 1);

    if (packedrecords <= 0)
    {
      ms_log (2, "trimrecord(): Cannot pack miniSEED record for %s %s\n", srcname, stime);
      return -2;
    }
  }

  msr_free (&datamsr);

  return 0;
} /* End of trimrecord() */

/***************************************************************************
 * writerecord():
 *
 * Used by trimrecord() to save repacked miniSEED to global record
 * buffer.
 ***************************************************************************/
static void
writerecord (char *record, int reclen, void *handlerdata)
{
  MSRecord *msr = handlerdata;
  Archive *arch;

  if (!record || reclen <= 0 || !handlerdata)
    return;

  /* Write to a single output file */
  if (ofp)
  {
    if (fwrite (record, reclen, 1, ofp) != 1)
    {
      ms_log (2, "Cannot write to '%s'\n", outputfile);
    }
  }

  /* Write to Archive(s) if specified and/or add to written list */
  if (archiveroot)
  {
    arch = archiveroot;
    while (arch)
    {
      ds_streamproc (&arch->datastream, msr, 0, verbose - 1);
      arch = arch->next;
    }
  }

  if (writtenfile)
  {
    MSTraceSeg *seg;

    if ((seg = mstl_addmsr (writtentl, msr, 1, 1, -1.0, -1.0)) == NULL)
    {
      ms_log (2, "Error adding MSRecord to MSTraceList, bah humbug.\n");
    }
    else
    {
      if (!seg->prvtptr)
      {
        if ((seg->prvtptr = malloc (sizeof (int64_t))) == NULL)
        {
          ms_log (2, "Error allocating memory for written count, bah humbug.\n");
        }
        else
        {
          *((int64_t *)seg->prvtptr) = 0;
        }
      }

      *((int64_t *)seg->prvtptr) += msr->reclen;
    }
  }

  totalrecsout++;
  totalbytesout += reclen;
} /* End of writerecord() */

/***************************************************************************
 * findselectlimits():
 *
 * Determine selection time limits for the given record based on all
 * matching selection entries.
 *
 * Return 0 on success and -1 on error.
 ***************************************************************************/
static int
findselectlimits (Selections *select, char *srcname, hptime_t starttime,
                  hptime_t endtime, hptime_t *selectstart, hptime_t *selectend)
{
  SelectTime *selecttime;
  char timestring[100];

  if (!select || !srcname || !selectstart || !selectend)
    return -1;

  *selectstart = HPTERROR;
  *selectend = HPTERROR;

  while ((select = ms_matchselect (select, srcname, starttime, endtime, &selecttime)))
  {
    while (selecttime)
    {
      /* Continue if selection edge time does not intersect with record coverage */
      if ((starttime < selecttime->starttime && !(starttime <= selecttime->starttime && endtime >= selecttime->starttime)))
      {
        selecttime = selecttime->next;
        continue;
      }
      else if ((endtime > selecttime->endtime && !(starttime <= selecttime->endtime && endtime >= selecttime->endtime)))
      {
        selecttime = selecttime->next;
        continue;
      }

      /* Check that the selection intersects previous selection range if set,
       * otherwise the combined selection is not possible. */
      if (*selectstart != HPTERROR && *selectend != HPTERROR &&
          !(*selectstart <= selecttime->endtime && *selectend >= selecttime->starttime))
      {
        ms_hptime2mdtimestr (starttime, timestring, 1);
        ms_log (1, "Warning: impossible combination of selections for record (%s, %s), not pruning.\n",
                srcname, timestring);
        *selectstart = HPTERROR;
        *selectend = HPTERROR;
        return 0;
      }

      if (*selectstart == HPTERROR || *selectstart > selecttime->starttime)
      {
        *selectstart = selecttime->starttime;
      }

      if (*selectend == HPTERROR || *selectend < selecttime->endtime)
      {
        *selectend = selecttime->endtime;
      }

      /* Shortcut if the entire record is already selected */
      if (starttime >= *selectstart && endtime <= *selectend)
        return 0;

      selecttime = selecttime->next;
    }

    select = select->next;
  }

  return 0;
} /* End of findselectlimits() */

/***************************************************************************
 * printwritten():
 *
 * Print summary of output records.
 ***************************************************************************/
static void
printwritten (MSTraceList *mstl)
{
  MSTraceID *id = 0;
  MSTraceSeg *seg = 0;
  char stime[30];
  char etime[30];
  FILE *ofp;

  if (!mstl)
    return;

  if (strcmp (writtenfile, "-") == 0)
  {
    ofp = stdout;
  }
  else if (strcmp (writtenfile, "--") == 0)
  {
    ofp = stderr;
  }
  else if ((ofp = fopen (writtenfile, "ab")) == NULL)
  {
    ms_log (2, "Cannot open output file: %s (%s)\n",
            writtenfile, strerror (errno));
    return;
  }

  /* Loop through trace list */
  id = mstl->traces;
  while (id)
  {
    /* Loop through segment list */
    seg = id->first;
    while (seg)
    {
      if (ms_hptime2seedtimestr (seg->starttime, stime, 1) == NULL)
        ms_log (2, "Cannot convert trace start time for %s\n", id->srcname);

      if (ms_hptime2seedtimestr (seg->endtime, etime, 1) == NULL)
        ms_log (2, "Cannot convert trace end time for %s\n", id->srcname);

      fprintf (ofp, "%s%s|%s|%s|%s|%c|%-24s|%-24s|%lld|%lld\n",
               (writtenprefix) ? writtenprefix : "",
               id->network, id->station, id->location, id->channel, id->dataquality,
               stime, etime, (long long int)*((int64_t *)seg->prvtptr),
               (long long int)seg->samplecnt);

      seg = seg->next;
    }

    id = id->next;
  }

  if (ofp != stdout && fclose (ofp))
    ms_log (2, "Cannot close output file: %s (%s)\n",
            writtenfile, strerror (errno));

} /* End of printwritten() */

/***************************************************************************
 * processparam():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processparam (int argcount, char **argvec)
{
  int optind;
  char *selectfile = 0;
  char *matchpattern = 0;
  char *rejectpattern = 0;
  char *tptr;

  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
  {
    if (strcmp (argvec[optind], "-V") == 0)
    {
      ms_log (1, "%s version: %s\n", PACKAGE, VERSION);
      exit (0);
    }
    else if (strcmp (argvec[optind], "-h") == 0)
    {
      usage (0);
      exit (0);
    }
    else if (strcmp (argvec[optind], "-H") == 0)
    {
      usage (1);
      exit (0);
    }
    else if (strncmp (argvec[optind], "-v", 2) == 0)
    {
      verbose += strspn (&argvec[optind][1], "v");
    }
    else if (strcmp (argvec[optind], "-s") == 0)
    {
      selectfile = getoptval (argcount, argvec, optind++);
    }
    else if (strcmp (argvec[optind], "-ts") == 0)
    {
      starttime = ms_seedtimestr2hptime (getoptval (argcount, argvec, optind++));
      if (starttime == HPTERROR)
        return -1;
    }
    else if (strcmp (argvec[optind], "-te") == 0)
    {
      endtime = ms_seedtimestr2hptime (getoptval (argcount, argvec, optind++));
      if (endtime == HPTERROR)
        return -1;
    }
    else if (strcmp (argvec[optind], "-M") == 0)
    {
      matchpattern = strdup (getoptval (argcount, argvec, optind++));
    }
    else if (strcmp (argvec[optind], "-R") == 0)
    {
      rejectpattern = strdup (getoptval (argcount, argvec, optind++));
    }
    else if (strcmp (argvec[optind], "-m") == 0)
    {
      tptr = getoptval (argcount, argvec, optind++);

      if (ms_addselect (&selections, tptr, HPTERROR, HPTERROR) < 0)
      {
        ms_log (2, "Unable to add selection: '%s'\n", tptr);
        return -1;
      }
    }
    else if (strcmp (argvec[optind], "-o") == 0)
    {
      outputfile = getoptval (argcount, argvec, optind++);
      outputmode = 0;
    }
    else if (strcmp (argvec[optind], "+o") == 0)
    {
      outputfile = getoptval (argcount, argvec, optind++);
      outputmode = 1;
    }
    else if (strcmp (argvec[optind], "-A") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), NULL) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-Ps") == 0 || strcmp (argvec[optind], "-P") == 0)
    {
      prunedata = 's';
    }
    else if (strcmp (argvec[optind], "-out") == 0)
    {
      writtenfile = getoptval (argcount, argvec, optind++);
    }
    else if (strcmp (argvec[optind], "-outprefix") == 0)
    {
      writtenprefix = getoptval (argcount, argvec, optind++);
    }
    else if (strcmp (argvec[optind], "-CHAN") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), CHANLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-QCHAN") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), QCHANLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-CDAY") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), CDAYLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-SDAY") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), SDAYLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-BUD") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), BUDLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-SDS") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), SDSLAYOUT) == -1)
        return -1;
    }
    else if (strcmp (argvec[optind], "-CSS") == 0)
    {
      if (addarchive (getoptval (argcount, argvec, optind++), CSSLAYOUT) == -1)
        return -1;
    }
    else if (strncmp (argvec[optind], "-", 1) == 0 &&
             strlen (argvec[optind]) > 1)
    {
      ms_log (2, "Unknown option: %s\n", argvec[optind]);
      exit (1);
    }
    else
    {
      tptr = argvec[optind];

      /* Check for an input file list */
      if (tptr[0] == '@')
      {
        if (addlistfile (tptr + 1) < 0)
        {
          ms_log (2, "Error adding list file %s", tptr + 1);
          exit (1);
        }
      }
      /* Otherwise this is an input file */
      else
      {
        /* Add file to global file list */
        if (addfile (tptr))
        {
          ms_log (2, "Error adding file to input list %s", tptr);
          exit (1);
        }
      }
    }
  }

  /* Make sure input file(s) were specified */
  if (filelist == 0)
  {
    ms_log (2, "No input files were specified\n\n");
    ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
    ms_log (1, "Try %s -h for usage\n", PACKAGE);
    exit (0);
  }

  /* Make sure output file(s) were specified */
  if (archiveroot == 0 && outputfile == 0)
  {
    ms_log (2, "No output files were specified\n\n");
    ms_log (1, "%s version %s\n\n", PACKAGE, VERSION);
    ms_log (1, "Try %s -h for usage\n", PACKAGE);
    exit (0);
  }

  /* Read data selection file */
  if (selectfile)
  {
    if (ms_readselectionsfile (&selections, selectfile) < 0)
    {
      ms_log (2, "Cannot read data selection file\n");
      exit (1);
    }
  }

  /* Expand match pattern from a file if prefixed by '@' */
  if (matchpattern)
  {
    if (*matchpattern == '@')
    {
      tptr = strdup (matchpattern + 1); /* Skip the @ sign */
      free (matchpattern);
      matchpattern = 0;

      if (readregexfile (tptr, &matchpattern) <= 0)
      {
        ms_log (2, "Cannot read match pattern regex file\n");
        exit (1);
      }

      free (tptr);
    }
  }

  /* Expand reject pattern from a file if prefixed by '@' */
  if (rejectpattern)
  {
    if (*rejectpattern == '@')
    {
      tptr = strdup (rejectpattern + 1); /* Skip the @ sign */
      free (rejectpattern);
      rejectpattern = 0;

      if (readregexfile (tptr, &rejectpattern) <= 0)
      {
        ms_log (2, "Cannot read reject pattern regex file\n");
        exit (1);
      }

      free (tptr);
    }
  }

  /* Compile match and reject patterns */
  if (matchpattern)
  {
    if (!(match = (regex_t *)malloc (sizeof (regex_t))))
    {
      ms_log (2, "Cannot allocate memory for match expression\n");
      exit (1);
    }

    if (regcomp (match, matchpattern, REG_EXTENDED) != 0)
    {
      ms_log (2, "Cannot compile match regex: '%s'\n", matchpattern);
    }

    free (matchpattern);
  }

  if (rejectpattern)
  {
    if (!(reject = (regex_t *)malloc (sizeof (regex_t))))
    {
      ms_log (2, "Cannot allocate memory for reject expression\n");
      exit (1);
    }

    if (regcomp (reject, rejectpattern, REG_EXTENDED) != 0)
    {
      ms_log (2, "Cannot compile reject regex: '%s'\n", rejectpattern);
    }

    free (rejectpattern);
  }

  /* Report the program version */
  if (verbose)
    ms_log (1, "%s version: %s\n", PACKAGE, VERSION);

  return 0;
} /* End of processparam() */

/***************************************************************************
 * getoptval:
 * Return the value to a command line option; checking that the value is
 * itself not an option (starting with '-') and is not past the end of
 * the argument list.
 *
 * argcount: total arguments in argvec
 * argvec: argument list
 * argopt: index of option to process, value is expected to be at argopt+1
 *
 * Returns value on success and exits with error message on failure
 ***************************************************************************/
static char *
getoptval (int argcount, char **argvec, int argopt)
{
  if (argvec == NULL || argvec[argopt] == NULL)
  {
    ms_log (2, "getoptval(): NULL option requested\n");
    exit (1);
    return 0;
  }

  /* Special case of '-o -' usage */
  if ((argopt + 1) < argcount && strcmp (argvec[argopt], "-o") == 0)
    if (strcmp (argvec[argopt + 1], "-") == 0)
      return argvec[argopt + 1];

  /* Special case of '+o -' usage */
  if ((argopt + 1) < argcount && strcmp (argvec[argopt], "+o") == 0)
    if (strcmp (argvec[argopt + 1], "-") == 0)
      return argvec[argopt + 1];

  /* Special case of '-s -' usage */
  if ((argopt + 1) < argcount && strcmp (argvec[argopt], "-s") == 0)
    if (strcmp (argvec[argopt + 1], "-") == 0)
      return argvec[argopt + 1];

  /* Special case of '-out -' or '-out --' usage */
  if ((argopt + 1) < argcount && strcmp (argvec[argopt], "-out") == 0)
    if (strcmp (argvec[argopt + 1], "-") == 0 ||
        strcmp (argvec[argopt + 1], "--") == 0)
      return argvec[argopt + 1];

  if ((argopt + 1) < argcount && *argvec[argopt + 1] != '-')
    return argvec[argopt + 1];

  ms_log (2, "Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
} /* End of getoptval() */

/***************************************************************************
 * setofilelimit:
 *
 * Check the current open file limit and if it is not >= 'limit' try
 * to increase it to 'limit'.
 *
 * Returns the open file limit on success and -1 on error.
 ***************************************************************************/
static int
setofilelimit (int limit)
{
  struct rlimit rlim;
  rlim_t oldlimit;

  /* Get the current soft open file limit */
  if (getrlimit (RLIMIT_NOFILE, &rlim) == -1)
  {
    ms_log (2, "getrlimit() failed to get open file limit\n");
    return -1;
  }

  if (rlim.rlim_cur < limit)
  {
    oldlimit = rlim.rlim_cur;
    rlim.rlim_cur = (rlim_t)limit;

    if (verbose > 1)
      ms_log (1, "Setting open file limit to %d\n",
              (int)rlim.rlim_cur);

    if (setrlimit (RLIMIT_NOFILE, &rlim) == -1)
    {
      ms_log (2, "setrlimit failed to raise open file limit from %d to %d (max: %d)\n",
              (int)oldlimit, (int)limit, rlim.rlim_max);
      return -1;
    }
  }

  return (int)rlim.rlim_cur;
} /* End of setofilelimit() */

/***************************************************************************
 * addfile:
 *
 * Add file to end of the specified file list.
 *
 * Check for and parse start and end byte offsets (a read range)
 * embedded in the file name.  The form for specifying a read range is:
 *  filename@startoffset:endoffset
 * where both start and end offsets are optional.

 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
addfile (char *filename)
{
  Filelink *newlp;
  char *at;
  char *colon;

  if (!filename)
  {
    ms_log (2, "addfile(): No file name specified\n");
    return -1;
  }

  if (!(newlp = (Filelink *)calloc (1, sizeof (Filelink))))
  {
    ms_log (2, "addfile(): Cannot allocate memory\n");
    return -1;
  }

  /* Check for optional read byte range specifiers
   * Expected form: "filename@startoffset:endoffset"
   * Both start are optional */
  if ((at = strrchr (filename, '@')))
  {
    *at++ = '\0';

    if ((colon = strrchr (at, ':')))
    {
      *colon++ = '\0';
      newlp->endoffset = strtoull (colon, NULL, 10);
    }

    newlp->startoffset = strtoull (at, NULL, 10);
  }

  if (!(newlp->filename = strdup (filename)))
  {
    ms_log (2, "addfile(): Cannot duplicate string\n");
    return -1;
  }

  /* Add new file to the end of the list */
  if (filelisttail == 0)
  {
    filelist = newlp;
    filelisttail = newlp;
  }
  else
  {
    filelisttail->next = newlp;
    filelisttail = newlp;
  }

  return 0;
} /* End of addfile() */

/***************************************************************************
 * addlistfile:
 *
 * Add files listed in the specified file to the global input file list.
 *
 * Returns count of files added on success and -1 on error.
 ***************************************************************************/
static int
addlistfile (char *filename)
{
  FILE *fp;
  char filelistent[1024];
  int filecount = 0;

  if (verbose >= 1)
    ms_log (1, "Reading list file '%s'\n", filename);

  if (!(fp = fopen (filename, "rb")))
  {
    ms_log (2, "Cannot open list file %s: %s\n", filename, strerror (errno));
    return -1;
  }

  while (fgets (filelistent, sizeof (filelistent), fp))
  {
    char *cp;

    /* End string at first newline character */
    if ((cp = strchr (filelistent, '\n')))
      *cp = '\0';

    /* Skip empty lines */
    if (!strlen (filelistent))
      continue;

    /* Skip comment lines */
    if (*filelistent == '#')
      continue;

    if (verbose > 1)
      ms_log (1, "Adding '%s' from list file\n", filelistent);

    if (addfile (filelistent))
      return -1;

    filecount++;
  }

  fclose (fp);

  return filecount;
} /* End of addlistfile() */

/***************************************************************************
 * addarchive:
 * Add entry to the data stream archive chain.  'layout' if defined
 * will be appended to 'path'.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
addarchive (const char *path, const char *layout)
{
  Archive *newarch;
  int pathlayout;

  if (!path)
  {
    ms_log (2, "addarchive(): cannot add archive with empty path\n");
    return -1;
  }

  if (!(newarch = (Archive *)malloc (sizeof (Archive))))
  {
    ms_log (2, "addarchive(): cannot allocate memory for new archive definition\n");
    return -1;
  }

  /* Setup new entry and add it to the front of the chain */
  pathlayout = strlen (path) + 2;
  if (layout)
    pathlayout += strlen (layout);

  if (!(newarch->datastream.path = (char *)malloc (pathlayout)))
  {
    ms_log (2, "addarchive(): cannot allocate memory for new archive path\n");
    if (newarch)
      free (newarch);
    return -1;
  }

  if (layout)
    snprintf (newarch->datastream.path, pathlayout, "%s/%s", path, layout);
  else
    snprintf (newarch->datastream.path, pathlayout, "%s", path);

  newarch->datastream.idletimeout = 60;
  newarch->datastream.grouproot = NULL;

  newarch->next = archiveroot;
  archiveroot = newarch;

  return 0;
} /* End of addarchive() */

/***************************************************************************
 * readregexfile:
 *
 * Read a list of regular expressions from a file and combine them
 * into a single, compound expression which is returned in *pppattern.
 * The return buffer is reallocated as need to hold the growing
 * pattern.  When called *pppattern should not point to any associated
 * memory.
 *
 * Returns the number of regexes parsed from the file or -1 on error.
 ***************************************************************************/
static int
readregexfile (char *regexfile, char **pppattern)
{
  FILE *fp;
  char line[1024];
  char linepattern[1024];
  int regexcnt = 0;
  int lengthbase;
  int lengthadd;

  if (!regexfile)
  {
    ms_log (2, "readregexfile: regex file not supplied\n");
    return -1;
  }

  if (!pppattern)
  {
    ms_log (2, "readregexfile: pattern string buffer not supplied\n");
    return -1;
  }

  /* Open the regex list file */
  if ((fp = fopen (regexfile, "rb")) == NULL)
  {
    ms_log (2, "Cannot open regex list file %s: %s\n",
            regexfile, strerror (errno));
    return -1;
  }

  if (verbose)
    ms_log (1, "Reading regex list from %s\n", regexfile);

  *pppattern = NULL;

  while ((fgets (line, sizeof (line), fp)) != NULL)
  {
    /* Trim spaces and skip if empty lines */
    if (sscanf (line, " %s ", linepattern) != 1)
      continue;

    /* Skip comment lines */
    if (*linepattern == '#')
      continue;

    regexcnt++;

    /* Add regex to compound regex */
    if (*pppattern)
    {
      lengthbase = strlen (*pppattern);
      lengthadd = strlen (linepattern) + 4; /* Length of addition plus 4 characters: |()\0 */

      *pppattern = realloc (*pppattern, lengthbase + lengthadd);

      if (*pppattern)
      {
        snprintf ((*pppattern) + lengthbase, lengthadd, "|(%s)", linepattern);
      }
      else
      {
        ms_log (2, "Cannot allocate memory for regex string\n");
        return -1;
      }
    }
    else
    {
      lengthadd = strlen (linepattern) + 3; /* Length of addition plus 3 characters: ()\0 */

      *pppattern = malloc (lengthadd);

      if (*pppattern)
      {
        snprintf (*pppattern, lengthadd, "(%s)", linepattern);
      }
      else
      {
        ms_log (2, "Cannot allocate memory for regex string\n");
        return -1;
      }
    }
  }

  fclose (fp);

  return regexcnt;
} /* End of readregexfile() */

/***************************************************************************
 * usage():
 * Print the usage message.
 ***************************************************************************/
static void
usage (int level)
{
  fprintf (stderr, "%s - filter miniSEED: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
           " ## Options ##\n"
           " -V           Report program version\n"
           " -h           Show this usage message\n"
           " -H           Show usage message with 'format' details (see -A option)\n"
           " -v           Be more verbose, multiple flags can be used\n"
           "\n"
           " ## Data selection options ##\n"
           " -s file      Specify a file containing selection criteria\n"
           " -ts time     Limit to records that contain or start after time\n"
           " -te time     Limit to records that contain or end before time\n"
           "                time format: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' delimiters: [,:.]\n"
           " -M match     Limit to records matching the specified regular expression\n"
           " -R reject    Limit to records not matching the specfied regular expression\n"
           "                Regular expressions are applied to: 'NET_STA_LOC_CHAN_QUAL'\n"
           "\n"
           " ## Output options ##\n"
           " -o file      Specify a single output file, use +o file to append\n"
           " -A format    Write all records in a custom directory/file layout (try -H)\n"
           " -Ps          Prune/trim records at the sample level\n"
           "\n"
           " ## Diagnostic output ##\n"
           " -out file    Write a summary of output records to specified file\n"
           " -outprefix X Include prefix on summary output lines for identification\n"
           "\n"
           " ## Input data ##\n"
           " file#        Files(s) of miniSEED records\n"
           "\n");

  if (level)
  {
    fprintf (stderr,
             "\n"
             "  # Preset format layouts #\n"
             " -CHAN dir    Write records into separate Net.Sta.Loc.Chan files\n"
             " -QCHAN dir   Write records into separate Net.Sta.Loc.Chan.Quality files\n"
             " -CDAY dir    Write records into separate Net.Sta.Loc.Chan.Year:Yday:<time> files\n"
             " -SDAY dir    Write records into separate Net.Sta.Year:Yday files\n"
             " -BUD BUDdir  Write records in a BUD file layout\n"
             " -SDS SDSdir  Write records in a SDS file layout\n"
             " -CSS CSSdir  Write records in a CSS-like file layout\n"
             "\n"
             "The archive 'format' argument is expanded for each record using the\n"
             "following flags:\n"
             "\n"
             "  n : network code, white space removed\n"
             "  s : station code, white space removed\n"
             "  l : location code, white space removed\n"
             "  c : channel code, white space removed\n"
             "  Y : year, 4 digits\n"
             "  y : year, 2 digits zero padded\n"
             "  j : day of year, 3 digits zero padded\n"
             "  H : hour, 2 digits zero padded\n"
             "  M : minute, 2 digits zero padded\n"
             "  S : second, 2 digits zero padded\n"
             "  F : fractional seconds, 4 digits zero padded\n"
             "  q : single character record quality indicator (D, R, Q, M)\n"
             "  L : data record length in bytes\n"
             "  r : Sample rate (Hz) as a rounded integer\n"
             "  R : Sample rate (Hz) as a float with 6 digit precision\n"
             "  %% : the percent (%%) character\n"
             "  # : the number (#) character\n"
             "\n"
             "The flags are prefaced with either the %% or # modifier.  The %% modifier\n"
             "indicates a defining flag while the # indicates a non-defining flag.\n"
             "All records with the same set of defining flags will be written to the\n"
             "same file. Non-defining flags will be expanded using the values in the\n"
             "first record for the resulting file name.\n"
             "\n");
  }
} /* End of usage() */
