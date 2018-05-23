# <p >datafilter - miniSEED data filtering</p>

1. [Name](#)
1. [Synopsis](#synopsis)
1. [Description](#description)
1. [Options](#options)
1. [Selection File](#selection-file)
1. [Input List File](#input-list-file)
1. [Input File Range](#input-file-range)
1. [Match Or Reject List File](#match-or-reject-list-file)
1. [Archive Format](#archive-format)
1. [Archive Format Examples](#archive-format-examples)
1. [Leap Second List File](#leap-second-list-file)
1. [Error Handling And Return Codes](#error-handling-and-return-codes)
1. [Author](#author)

## <a id='synopsis'>Synopsis</a>

<pre >
datafilter [options] file1 [file2 file3 ...]
</pre>

## <a id='description'>Description</a>

<p ><b>datafilter</b> filters miniSEED data.  Various data selection criteria are available including by identifier, time or lists of arbitrary identifiers and times.</p>

<p >By default, records that match all criteria and at least partially match a selected time range are written to the output.  Output data optionally may be pruned, i.e. trimmed, as the sample level.</p>

<p >Input files will be read and processed in the order specified.</p>

<p >Files on the command line prefixed with a '@' character are input list files and are expected to contain a simple list of input files, see <b>INPUT LIST FILE</b> for more details.</p>

<p >Each input file may be specified with an explict byte range to read. The program will begin reading at the specified start offset and stop reading at the specified end range.  See <b>INPUT FILE RANGE</b> for more details.</p>

## <a id='options'>Options</a>

<b>-V</b>

<p style="padding-left: 30px;">Print program version and exit.</p>

<b>-h</b>

<p style="padding-left: 30px;">Print program usage and exit.</p>

<b>-H</b>

<p style="padding-left: 30px;">Print verbose program usage including details of archive format specification and exit.</p>

<b>-v</b>

<p style="padding-left: 30px;">Be more verbose.  This flag can be used multiple times ("-v -v" or "-vv") for more verbosity.</p>

<b>-s </b><i>selectfile</i>

<p style="padding-left: 30px;">Limit processing to miniSEED records that match a selection in the specified file.  The selection file contains parameters to match the network, station, location, channel, quality and time range for input records.  As a special case, specifying "-" will result in selection lines being read from stdin.  For more details see the <b>SELECTION FILE</b> section below.</p>

<b>-ts </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to miniSEED records that start after or contain <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS.FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.), except the seconds and fractional seconds must be separated by a period (.).</p>

<b>-te </b><i>time</i>

<p style="padding-left: 30px;">Limit processing to miniSEED records that end before or contain <i>time</i>.  The format of the <i>time</i> argument is: 'YYYY[,DDD,HH,MM,SS.FFFFFF]' where valid delimiters are either commas (,), colons (:) or periods (.), except the seconds and fractional seconds must be separated by a period (.).</p>

<b>-M </b><i>match</i>

<p style="padding-left: 30px;">Limit input to records that match this regular expression, the <i>match</i> is tested against the full source name: 'NET_STA_LOC_CHAN_QUAL'.  If the match expression begins with an '@' character it is assumed to indicate a file containing a list of expressions to match, see the <b>MATCH OR REJECT LIST FILE</b> section below.</p>

<b>-R </b><i>reject</i>

<p style="padding-left: 30px;">Limit input to records that do not match this regular expression, the <i>reject</i> is tested against the full source name: 'NET_STA_LOC_CHAN_QUAL'.  If the reject expression begins with an '@' character it is assumed to indicate a file containing a list of expressions to reject, see the <b>MATCH OR REJECT LIST FILE</b> section below.</p>

<b>-m </b><i>match</i>

<p style="padding-left: 30px;">This is effectively the same as <b>-M</b> except that <i>match</i> is evaluated as a globbing expression instead of regular expression. Otherwise undocumented as it is primarily useful at the IRIS DMC.</p>

<b>-o </b><i>file</i>

<p style="padding-left: 30px;">Write all output data to output <i>file</i>.  If '-' is specified as the output file all output data will be written to standard out.  By default the output file will be overwritten, changing the option to <i>+o file</i> appends to the output file.</p>

<b>-A </b><i>format</i>

<p style="padding-left: 30px;">All output records will be written to a directory/file layout defined by <i>format</i>.  All directories implied in the <i>format</i> string will be created if necessary.  The option may be used multiple times to write input records to multiple archives.  See the <b>ARCHIVE FORMAT</b> section below for more details including pre-defined archive layouts.</p>

<b>-CHAN </b><i>directory</i>

<b>-QCHAN </b><i>directory</i>

<b>-CDAY </b><i>directory</i>

<b>-SDAY </b><i>directory</i>

<b>-BUD </b><i>directory</i>

<b>-SDS </b><i>directory</i>

<b>-CSS </b><i>directory</i>

<p style="padding-left: 30px;">Pre-defined output archive formats, see the <b>Archive Format</b> section below for more details.</p>

<b>-Ps</b>

<p style="padding-left: 30px;">Prune, i.e. trim, records at the sample level according to the time range criteria.  Record trimming requires a supported data encoding, if unsupported (primarily older encodings) the record will be in the output untrimmed.</p>

<b>-out file</b>

<p style="padding-left: 30px;">Print a summary of output records to the specified file.  Any existing file will be appended to.  Specify the file as '-' to print to stdout or '--' to print to stderr.  Each line contains network, station, location, channel, quality, start time, end time, byte count and sample count for each output trace segment.</p>

<b>-outprefix prefix</b>

<p style="padding-left: 30px;">Include the specified prefix string at the beginning of each line of summary output when using the <i>-out</i> option.  This is useful to identify the summary output in a stream that is potentially mixed with other output.</p>

## <a id='selection-file'>Selection File</a>

<p >A selection file is used to match input data records based on network, station, location and channel information.  Optionally a quality and time range may also be specified for more refined selection.  The non-time fields may use the '*' wildcard to match multiple characters and the '?' wildcard to match single characters.  Character sets may also be used, for example '[ENZ]' will match either E, N or Z. The '#' character indicates the remaining portion of the line will be ignored.</p>

<p >Example selection file entires (the first four fields are required)</p>
<pre >
#net sta  loc  chan  qual  start             end
IU   ANMO *    BH?
II   *    *    *     Q
IU   COLA 00   LH[ENZ] R
IU   COLA 00   LHZ   *     2008,100,10,00,00 2008,100,10,30,00
</pre>

<p ><b>Warning:</b> with a selection file it is possible to specify multiple, arbitrary selections.  Some combinations of these selects are not possible.  See <b>CAVEATS AND LIMITATIONS</b> for more details.</p>

## <a id='input-list-file'>Input List File</a>

<p >A list file can be used to specify input files, one file per line. The initial '@' character indicating a list file is not considered part of the file name.  As an example, if the following command line option was used:</p>

<pre >
<b>@files.list</b>
</pre>

<p >The 'files.list' file might look like this:</p>

<pre >
data/day1.mseed
data/day2.mseed
data/day3.mseed
</pre>

## <a id='input-file-range'>Input File Range</a>

<p >Each input file may be specified with an associated byte range to read.  The program will begin reading at the specified start offset and finish reading when at or beyond the end offset.  The range is specified by appending an '@' charater to the filename with the start and end offsets separated by a colon:</p>

<pre >
filename.mseed@[startoffset][:][endoffset]
</pre>

<p >For example: "filename.mseed@4096:8192".  Both the start and end offsets are optional.  The colon separator is optional if no end offset is specified.</p>

## <a id='match-or-reject-list-file'>Match Or Reject List File</a>

<p >A list file used with either the <b>-M</b> or <b>-R</b> contains a list of regular expressions (one on each line) that will be combined into a single compound expression.  The initial '@' character indicating a list file is not considered part of the file name.  As an example, if the following command line option was used:</p>

<pre >
<b>-M @match.list</b>
</pre>

<p >The 'match.list' file might look like this:</p>

<pre >
IU_ANMO_.*
IU_ADK_00_BHZ.*
II_BFO_00_BHZ_Q
</pre>

## <a id='archive-format'>Archive Format</a>

<p >The pre-defined archive layouts are as follows:</p>

<pre >
-CHAN dir   :: dir/%n.%s.%l.%c
-QCHAN dir  :: dir/%n.%s.%l.%c.%q
-CDAY dir   :: dir/%n.%s.%l.%c.%Y:%j:#H:#M:#S
-SDAY dir   :: dir/%n.%s.%Y:%j
-BUD dir    :: dir/%n/%s/%s.%n.%l.%c.%Y.%j
-SDS dir    :: dir/%Y/%n/%s/%c.D/%n.%s.%l.%c.D.%Y.%j
-CSS dir    :: dir/%Y/%j/%s.%c.%Y:%j:#H:#M:#S
</pre>

<p >An archive format is expanded for each record using the following substitution flags:</p>

<pre >
  <b>n</b> : network code, white space removed
  <b>s</b> : station code, white space removed
  <b>l</b> : location code, white space removed
  <b>c</b> : channel code, white space removed
  <b>Y</b> : year, 4 digits
  <b>y</b> : year, 2 digits zero padded
  <b>j</b> : day of year, 3 digits zero padded
  <b>H</b> : hour, 2 digits zero padded
  <b>M</b> : minute, 2 digits zero padded
  <b>S</b> : second, 2 digits zero padded
  <b>F</b> : fractional seconds, 4 digits zero padded
  <b>q</b> : single character record quality indicator (D, R, Q)
  <b>L</b> : data record length in bytes
  <b>r</b> : sample rate (Hz) as a rounded integer
  <b>R</b> : sample rate (Hz) as a float with 6 digit precision
  <b>%</b> : the percent (%) character
  <b>#</b> : the number (#) character
</pre>

<p >The flags are prefaced with either the <b>%</b> or <b>#</b> modifier. The <b>%</b> modifier indicates a defining flag while the <b>#</b> indicates a non-defining flag.  All records with the same set of defining flags will be written to the same file.  Non-defining flags will be expanded using the values in the first record for the resulting file name.</p>

<p >Time flags are based on the start time of the given record.</p>

## <a id='archive-format-examples'>Archive Format Examples</a>

<p >The format string for the predefined <i>BUD</i> layout:</p>

<p ><b>/archive/%n/%s/%s.%n.%l.%c.%Y.%j</b></p>

<p >would expand to day length files named something like:</p>

<p ><b>/archive/NL/HGN/HGN.NL..BHE.2003.055</b></p>

<p >As an example of using non-defining flags the format string for the predefined <i>CSS</i> layout:</p>

<p ><b>/data/%Y/%j/%s.%c.%Y:%j:#H:#M:#S</b></p>

<p >would expand to:</p>

<p ><b>/data/2003/055/HGN.BHE.2003:055:14:17:54</b></p>

<p >resulting in day length files because the hour, minute and second are specified with the non-defining modifier.  The hour, minute and second fields are from the first record in the file.</p>

## <a id='leap-second-list-file'>Leap Second List File</a>

<p >If the environment variable LIBMSEED_LEAPSECOND_FILE is set it is expected to indicate a file containing a list of leap seconds as published by NIST and IETF, usually available here: https://www.ietf.org/timezones/data/leap-seconds.list</p>

<p >Specifying this file is highly recommended when pruning records at the sample level.</p>

<p >If present, the leap seconds listed in this file will be used to adjust the time coverage for records that contain a leap second. Also, leap second indicators in the miniSEED headers will be ignored.</p>

<p >To suppress the warning printed by the program without specifying a leap second file, set LIBMSEED_LEAPSECOND_FILE=NONE.</p>

## <a id='error-handling-and-return-codes'>Error Handling And Return Codes</a>

<p >Any significant error message will be pre-pended with "ERROR" which can be parsed to determine run-time errors.  Additionally the program will return an exit code of 0 on successful operation and 1 when any errors were encountered.</p>

## <a id='author'>Author</a>

<pre >
Chad Trabant
IRIS Data Management Center
</pre>


(man page 2018/5/22)
