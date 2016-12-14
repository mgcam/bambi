/*  decode.c -- index decoder subcommand.

    Copyright (C) 2016 Genome Research Ltd.

    Author: Jennifer Liddle <js10@sanger.ac.uk>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <bambi.h>
#include <assert.h>
#include <htslib/sam.h>
#include <string.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <regex.h>
#include <htslib/khash.h>
#include <cram/sam_header.h>

#define DEFAULT_MAX_LOW_QUALITY_TO_CONVERT 15
#define DEFAULT_MAX_NO_CALLS 2
#define DEFAULT_MAX_MISMATCHES 1
#define DEFAULT_MIN_MISMATCH_DELTA 1
#define DEFAULT_BARCODE_TAG "BC"
#define DEFAULT_QUALITY_TAG "QT"

/*
 * structure to hold options
 */
typedef struct {
    char* input_name;
    char* output_name;
    char* barcode_name;
    char *metrics_name;
    char *barcode_tag_name;
    char *quality_tag_name;
    bool verbose;
    int max_low_quality_to_convert;
    bool convert_low_quality;
    int max_no_calls;
    int max_mismatches;
    int min_mismatch_delta;
    bool change_read_name;
    char *argv_list;
    char *input_fmt;
    char *output_fmt;
    char compression_level;
} opts_t;

static void free_opts(opts_t* opts)
{
    if (!opts) return;
    free(opts->input_name);
    free(opts->output_name);
    free(opts->barcode_name);
    free(opts->barcode_tag_name);
    free(opts->quality_tag_name);
    free(opts->argv_list);
    free(opts->input_fmt);
    free(opts->output_fmt);
    free(opts);
}

/*
 * details read from barcode file
 * Plus metrics information for each barcode
 */
typedef struct {
    char *seq;
    char *name;
    char *lib;
    char *sample;
    char *desc;
    int reads, pf_reads, perfect, pf_perfect, one_mismatch, pf_one_mismatch;
} bc_details_t;

// Barcode array
typedef struct {
    int end;    // number of entries in the barcode entry
    int max;    // how big the array is
    int tag_len;    // convenient place to store this
    bc_details_t **entries;    // array of barcodes
} bc_array_t;

/*
 * Barcode array functions
 */
void bc_push(bc_array_t *bc, bc_details_t *bcd) 
{
    if (bc->end == bc->max) {
        // expand the array
        bc->max *= 2;
        bc->entries = realloc(bc->entries, bc->max * sizeof(bc_details_t *));
    }
    bc->entries[bc->end] = bcd;
    bc->end++;
}

void bc_free(bc_array_t *bc)
{
    int n;
    for (n=0; n < bc->end; n++) {
        bc_details_t *bcd = bc->entries[n];
        free(bcd->seq);
        free(bcd->name);
        free(bcd->lib);
        free(bcd->sample);
        free(bcd->desc);
        free(bcd);
    }
    free(bc->entries);
    free(bc);
}

bc_array_t *bc_init(void)
{
    bc_array_t *bc = calloc(1,sizeof(bc_array_t));
    bc->end = 0;
    bc->max = 100;
    bc->tag_len = 0;
    bc->entries = calloc(bc->max, sizeof(bc_details_t *));

    // initialise first entry for null metrics
    bc_details_t *bcd = calloc(1, sizeof(bc_details_t));
    bcd->seq = NULL;    // we can't initialise this until we know the tag_length
    bcd->name = strdup("0");
    bcd->lib = strdup("");
    bcd->sample = strdup("");
    bcd->desc = strdup("");
    bc_push(bc,bcd);
    return bc;
}

/*
 * display usage information
 */
static void usage(FILE *write_to)
{
    fprintf(write_to,
"Usage: bambi decode [options] filename\n"
"\n"
"Options:\n"
"  -o   --output                        output file [default: stdout]\n"
"  -v   --verbose                       verbose output\n"
"  -b   --barcode-file                  file containing barcodes\n"
"       --convert-low-quality           Convert low quality bases in barcode read to 'N'\n"
"       --max-low-quality-to-convert    Max low quality phred value to convert bases in barcode read to 'N'\n"
"       --max-no-calls                  Max allowable number of no-calls in a barcode read before it is considered unmatchable\n"
"       --max-mismatches                Maximum mismatches for a barcode to be considered a match\n"
"       --min-mismatch-delta            Minimum difference between number of mismatches in the best and second best barcodes for\n"
"                                       a barcode to be considered a match\n"
"       --change-read-name              Change the read name by adding #<barcode> suffix\n"
"       --metrics-file                  Per-barcode and per-lane metrics written to this file\n"
"       --barcode-tag-name              Barcode tag name [default: " DEFAULT_BARCODE_TAG "]\n"
"       --quality-tag-name              Quality tag name [default: " DEFAULT_QUALITY_TAG "]\n"
"       --input_fmt                     format of input file [sam/bam/cram]\n"
"       --output_fmt                    format of output file [sam/bam/cram]\n"
"       --compression_level             Compression level of output file [0..9]\n"
);
}

/*
 * Takes the command line options and turns them into something we can understand
 */
static opts_t* parse_args(int argc, char *argv[])
{
    if (argc == 1) { usage(stdout); return NULL; }

    const char* optstring = "i:o:vb:";

    static const struct option lopts[] = {
        { "input",                      1, 0, 'i' },
        { "output",                     1, 0, 'o' },
        { "verbose",                    0, 0, 'v' },
        { "max-low-quality-to-convert", 1, 0, 0 },
        { "convert-low-quality",        0, 0, 0 },
        { "barcode-file",               1, 0, 'b' },
        { "max-no-calls",               1, 0, 0 },
        { "max-mismatches",             1, 0, 0 },
        { "min-mismatch-delta",         1, 0, 0 },
        { "change-read-name",           0, 0, 0 },
        { "metrics-file",               1, 0, 0 },
        { "barcode-tag-name",           1, 0, 0 },
        { "quality-tag-name",           1, 0, 0 },
        { "input-fmt",                  1, 0, 0 },
        { "output-fmt",                 1, 0, 0 },
        { "compression-level",          1, 0, 0 },
        { NULL, 0, NULL, 0 }
    };

    opts_t* opts = calloc(sizeof(opts_t), 1);
    if (!opts) { perror("cannot allocate option parsing memory"); return NULL; }

    opts->argv_list = stringify_argv(argc+1, argv-1);
    if (opts->argv_list[strlen(opts->argv_list)-1] == ' ') opts->argv_list[strlen(opts->argv_list)-1] = 0;

    // set defaults
    opts->max_low_quality_to_convert = DEFAULT_MAX_LOW_QUALITY_TO_CONVERT;
    opts->max_no_calls = DEFAULT_MAX_NO_CALLS;
    opts->max_mismatches = DEFAULT_MAX_MISMATCHES;
    opts->min_mismatch_delta = DEFAULT_MIN_MISMATCH_DELTA;
    opts->verbose = false;
    opts->convert_low_quality = false;
    opts->change_read_name = false;
    opts->barcode_tag_name = strdup(DEFAULT_BARCODE_TAG);
    opts->quality_tag_name = strdup(DEFAULT_QUALITY_TAG);
    
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, optstring, lopts, &option_index)) != -1) {
        const char *arg;
        switch (opt) {
        case 'i':   opts->input_name = strdup(optarg);
                    break;
        case 'o':   opts->output_name = strdup(optarg);
                    break;
        case 'v':   opts->verbose = true;
                    break;
        case 'b':   opts->barcode_name = strdup(optarg);
                    break;
        case 0:     arg = lopts[option_index].name;
                         if (strcmp(arg, "metrics-file") == 0)               opts->metrics_name = strdup(optarg);
                    else if (strcmp(arg, "max-low-quality-to-convert") == 0) opts->max_low_quality_to_convert = atoi(optarg);
                    else if (strcmp(arg, "convert-low-quality") == 0)        opts->convert_low_quality = true;
                    else if (strcmp(arg, "max-no-calls") == 0)               opts->max_no_calls = atoi(optarg);
                    else if (strcmp(arg, "max-mismatches") == 0)             opts->max_mismatches = atoi(optarg);
                    else if (strcmp(arg, "min-mismatch-delta") == 0)         opts->min_mismatch_delta = atoi(optarg);
                    else if (strcmp(arg, "change-read-name") == 0)           opts->change_read_name = true;
                    else if (strcmp(arg, "barcode-tag-name") == 0)           opts->barcode_tag_name = strdup(optarg);
                    else if (strcmp(arg, "quality-tag-name") == 0)           opts->quality_tag_name = strdup(optarg);
                    else if (strcmp(arg, "input-fmt") == 0)                  opts->input_fmt = strdup(optarg);
                    else if (strcmp(arg, "output-fmt") == 0)                 opts->output_fmt = strdup(optarg);
                    else if (strcmp(arg, "compression-level") == 0)          opts->compression_level = *optarg;
                    else {
                        printf("\nUnknown option: %s\n\n", arg); 
                        usage(stdout); free_opts(opts);
                        return NULL;
                    }
                    break;
        default:    printf("Unknown option: '%c'\n", opt);
            /* else fall-through */
        case '?':   usage(stdout); free_opts(opts); return NULL;
        }
    }

    argc -= optind;
    argv += optind;

    if (argc > 0) opts->input_name = strdup(argv[0]);
    optind = 0;

    // some validation and tidying
    if (!opts->input_name) {
        fprintf(stderr,"You must specify an input file (-i or --input)\n");
        usage(stderr); free_opts(opts);
        return NULL;
    }
    if (!opts->barcode_name) {
        fprintf(stderr,"You must specify a barcode (tags) file (-b or --barcode-file)\n");
        usage(stderr); free_opts(opts);
        return NULL;
    }
    // output defaults to stdout
    if (!opts->output_name) opts->output_name = strdup("-");

    return opts;
}

//
// char *checkBarcodeQuality(char *barcode, char *quality);
//
// return a new barcode read string with low quality bases converted to 'N'
//
static char *checkBarcodeQuality(char * barcode, char *quality, int max_low_quality_to_convert)
{
    if (!quality) return strdup(barcode);

    if (!barcode || strlen(barcode) != strlen(quality)) {
        fprintf(stderr, "checkBarcodeQuality(): barcode and quality are different lengths\n");
        return NULL;
    }

    int mlq = max_low_quality_to_convert ? max_low_quality_to_convert : DEFAULT_MAX_LOW_QUALITY_TO_CONVERT;
    char *newBarcode = strdup(barcode);
    int i;
    for (i=0; i < strlen(quality); i++) {
        int qual = quality[i] - 33;

        if (qual <= mlq) {
            newBarcode[i] = 'N';
        } else {
            newBarcode[i] = barcode[i];
        }
    }

    return newBarcode;
}

void writeMetricsLine(FILE *f, bc_details_t *bcd, opts_t *opts, int total_reads, int max_reads, int total_pf_reads, int max_pf_reads, int total_pf_reads_assigned, int nReads)
{
    fprintf(f, "%s\t", bcd->seq);
    fprintf(f, "%s\t", bcd->name);
    fprintf(f, "%s\t", bcd->lib);
    fprintf(f, "%s\t", bcd->sample);
    fprintf(f, "%s\t", bcd->desc);
    fprintf(f, "%d\t", bcd->reads);
    fprintf(f, "%d\t", bcd->pf_reads);
    fprintf(f, "%d\t", bcd->perfect);
    fprintf(f, "%d\t", bcd->pf_perfect);
    fprintf(f, "%d\t", bcd->one_mismatch);
    fprintf(f, "%d\t", bcd->pf_one_mismatch);
    fprintf(f, "%f\t", total_reads ? bcd->reads / (double)total_reads : 0 );
    fprintf(f, "%f\t", max_reads ? bcd->reads / (double)max_reads : 0 );
    fprintf(f, "%f\t", total_pf_reads ? bcd->pf_reads / (double)total_pf_reads : 0 );
    fprintf(f, "%f\t", max_pf_reads ? bcd->pf_reads / (double)max_pf_reads : 0 );
    fprintf(f, "%f", total_pf_reads_assigned ? bcd->pf_reads * nReads / (double)total_pf_reads_assigned : 0);
    fprintf(f, "\n");

}

/*
 *
 */
int writeMetrics(bc_array_t *barcodeArray, opts_t *opts)
{
    bc_details_t *bcd = barcodeArray->entries[0];
    int total_reads = bcd->reads;
    int total_pf_reads = bcd->pf_reads;
    int total_pf_reads_assigned = 0;
    int max_reads = 0;
    int max_pf_reads = 0;
    int nReads = 0;
    int n;

    // Open the metrics file
    FILE *f = fopen(opts->metrics_name, "w");
    if (!f) {
        fprintf(stderr,"Can't open metrics file %s\n", opts->metrics_name);
        return 1;
    }

    // first loop to count things
    for (n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];;
        total_reads += bcd->reads;
        total_pf_reads += bcd->pf_reads;
        total_pf_reads_assigned += bcd->pf_reads;
        if (max_reads < bcd->reads) max_reads = bcd->reads;
        if (max_pf_reads < bcd->pf_reads) max_pf_reads = bcd->pf_reads;
        nReads++;
    }

    // print header
    fprintf(f, "##\n");
    fprintf(f, "# ");
    fprintf(f, "BARCODE_TAG_NAME=%s ", opts->barcode_tag_name);
    fprintf(f, "MAX_MISMATCHES=%d ", opts->max_mismatches);
    fprintf(f, "MIN_MISMATCH_DELTA=%d ", opts->min_mismatch_delta);
    fprintf(f, "MAX_NO_CALLS=%d ", opts->max_no_calls);
    fprintf(f, "\n");
    fprintf(f, "##\n");
    fprintf(f, "#\n");
    fprintf(f, "\n");
    fprintf(f, "##\n");

    fprintf(f, "BARCODE\t");
    fprintf(f, "BARCODE_NAME\t");
    fprintf(f, "LIBRARY_NAME\t");
    fprintf(f, "SAMPLE_NAME\t");
    fprintf(f, "DESCRIPTION\t");
    fprintf(f, "READS\t");
    fprintf(f, "PF_READS\t");
    fprintf(f, "PERFECT_MATCHES\t");
    fprintf(f, "PF_PERFECT_MATCHES\t");
    fprintf(f, "ONE_MISMATCH_MATCHES\t");
    fprintf(f, "PF_ONE_MISMATCH_MATCHES\t");
    fprintf(f, "PCT_MATCHES\t");
    fprintf(f, "RATIO_THIS_BARCODE_TO_BEST_BARCODE_PCT\t");
    fprintf(f, "PF_PCT_MATCHES\t");
    fprintf(f, "PF_RATIO_THIS_BARCODE_TO_BEST_BARCODE_PCT\t");
    fprintf(f, "PF_NORMALIZED_MATCHES\n");


    // second loop to print things
    for (n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];
        writeMetricsLine(f, bcd, opts, total_reads, max_reads, total_pf_reads, max_pf_reads, total_pf_reads_assigned, nReads);
    }
    // treat Tag 0 as a special case
    barcodeArray->entries[0]->perfect = 0;
    barcodeArray->entries[0]->pf_perfect = 0;
    barcodeArray->entries[0]->name[0] = 0;
    writeMetricsLine(f, barcodeArray->entries[0], opts, total_reads, max_reads, total_pf_reads, max_pf_reads, 0, nReads);

    fclose(f);
    return 0;
}

/*
 * Read the barcode file into a hash
 */
bc_array_t *loadBarcodeFile(char *barcode_name)
{
    bc_array_t *barcodeArray = bc_init();
    FILE *fh = fopen(barcode_name,"r");
    if (!fh) {
        fprintf(stderr,"ERROR: Can't open barcode file %s\n", barcode_name);
        return NULL;
    }
    
    char *buf = NULL;
    int tag_length = 0;
    size_t n;
    if (getline(&buf,&n,fh) < 0) {;    // burn first line which is a header
        fprintf(stderr,"ERROR: problem reading barcode file\n");
        return NULL;
    }
    free(buf); buf=NULL;

    while (getline(&buf, &n, fh) > 0) {
        char *s;
        if (buf[strlen(buf)-1] == '\n') buf[strlen(buf)-1]=0;   // remove trailing lf
        bc_details_t *bcd = calloc(1,sizeof(bc_details_t));
        s = strtok(buf,"\t");  bcd->seq     = strdup(s);
        s = strtok(NULL,"\t"); bcd->name    = strdup(s);
        s = strtok(NULL,"\t"); bcd->lib     = strdup(s);
        s = strtok(NULL,"\t"); bcd->sample  = strdup(s);
        s = strtok(NULL,"\t"); bcd->desc    = strdup(s);
        bc_push(barcodeArray,bcd);
        free(buf); buf=NULL;

        if (tag_length == 0) {
            tag_length = strlen(bcd->seq);
        } else {
            if (tag_length != strlen(bcd->seq)) {
                fprintf(stderr,"ERROR: Tag '%s' is a different length to the previous tag\n", bcd->seq);
                return NULL;
            }
        }
    }

    barcodeArray->tag_len = tag_length;
    barcodeArray->entries[0]->seq = calloc(1,tag_length+1);
    memset(barcodeArray->entries[0]->seq, 'N', tag_length);
    free(buf);
    fclose(fh);
    return barcodeArray;
}

/*
 * return true if base is a noCall
 */
int isNoCall(char b)
{
    return b=='N' || b=='n' || b=='.';
}

/*
 * Count the number of noCalls in a sequence
 */
static int noCalls(char *s)
{
    int n=0;
    while (*s) {
        if (isNoCall(*s++)) n++;
    }
    return n;
}
        
/*
 * count number of mismatches between two sequences
 * (ignoring noCalls)
 */
static int countMismatches(char *tag, char *barcode)
{
    char *t, *b;;
    int n = 0;
    for (t=tag, b=barcode; *t; t++, b++) {
        if (!isNoCall(*t)) {
            if (!isNoCall(*b)) {
                if (*t != *b) {
                    n++;
                }
            }
        }
    }
    return n;
}

/*
 * find the best match in the barcode (tag) file for a given barcode
 * return the tag, if a match found, else return NULL
 */
bc_details_t *findBestMatch(char *barcode, bc_array_t *barcodeArray, opts_t *opts)
{
    int bcLen = barcodeArray->tag_len;   // size of barcode sequence in barcode file
    bc_details_t *best_match = NULL;
    int nmBest = bcLen;             // number of mismatches (best)
    int nm2Best = bcLen;            // number of mismatches (second best)
    int nCalls = noCalls(barcode);
    int n;

    // for each tag in barcodeArray
    for (n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];

        int nMismatches = countMismatches(bcd->seq, barcode);
        if (nMismatches < nmBest) {
            if (best_match) nm2Best = nmBest;
            nmBest = nMismatches;
            best_match = bcd;
        } else {
            if (nMismatches < nm2Best) nm2Best = nMismatches;
        }
    }

    bool matched = best_match &&
                   nCalls <= opts->max_no_calls &&
                   nmBest <= opts->max_mismatches &&
                   nm2Best - nmBest >= opts->min_mismatch_delta;

    if (matched) return best_match;
    return barcodeArray->entries[0];
}

/*
 * Update the metrics information
 */
void updateMetrics(bc_details_t *bcd, char *seq, bool isPf)
{
    int n = 99;
    if (seq) n = countMismatches(bcd->seq, seq);

    bcd->reads++;
    if (isPf) bcd->pf_reads++;

    if (n==0) {     // count perfect matches
        bcd->perfect++;
        if (isPf) bcd->pf_perfect++;
    }

    if (n==1) {     // count out-by-one matches
        bcd->one_mismatch++;
        if (isPf) bcd->pf_one_mismatch++;
    }
        
}

/*
 * find the best match in the barcode (tag) file, and return the corresponding barcode name
 * return NULL if no match found
 */
static char *findBarcodeName(char *barcode, bc_array_t *barcodeArray, opts_t *opts, bool isPf)
{
    bc_details_t *bcd = findBestMatch(barcode, barcodeArray, opts);
    updateMetrics(bcd, barcode, isPf);
    return bcd->name;
}

/*
 * make a new tag by appending #<name> to the old tag
 */
char *makeNewTag(bam1_t *rec, char *tag, char *name)
{
    char *rg = "";
    uint8_t *p = bam_aux_get(rec,tag);
    if (p) rg = bam_aux2Z(p);
    char *newtag = malloc(strlen(rg)+1+strlen(name)+1);
    strcpy(newtag, rg);
    strcat(newtag,"#");
    strcat(newtag, name);
    return newtag;
}

/*
 * Change the read name by adding "#<suffix>"
 */
void add_suffix(bam1_t *rec, char *suffix)
{
    int oldqlen = strlen((char *)rec->data);
    int newlen = rec->l_data + strlen(suffix) + 1;

    if (newlen > rec->m_data) {
        rec->m_data = newlen;
        kroundup32(rec->m_data);
        rec->data = (uint8_t *)realloc(rec->data, rec->m_data);
    }
    memmove(rec->data + oldqlen + strlen(suffix) + 1,
            rec->data + oldqlen,
            rec->l_data - oldqlen);
    rec->data[oldqlen] = '#';
    memmove(rec->data + oldqlen + 1,
            suffix,
            strlen(suffix) + 1);
    rec->l_data = newlen;
    rec->core.l_qname += strlen(suffix) + 1;
}

/*
 * Add a new @RG line to the header
 */
void addNewRG(SAM_hdr *sh, char *entry, char *bcname, char *lib, char *sample, char *desc)
{
    char *saveptr;
    char *p = strtok_r(entry,"\t",&saveptr);
    char *newtag = malloc(strlen(p)+1+strlen(bcname)+1);
    strcpy(newtag, p);
    strcat(newtag,"#");
    strcat(newtag, bcname);
    sam_hdr_add(sh, "RG", "ID", newtag, NULL, NULL);

    SAM_hdr_type *hdr = sam_hdr_find(sh, "RG", "ID", newtag);
    while (1) {
        char *pu = NULL;
        char *t = strtok_r(NULL, ":", &saveptr);
        if (!t) break;
        char *v = strtok_r(NULL, "\t", &saveptr);
        if (!v) break;

        // handle special cases
        if (strcmp(t,"PU") == 0) {
            // add #bcname
            pu = malloc(strlen(v) + 1 + strlen(bcname) + 1);
            strcpy(pu, v); strcat(pu,"#"); strcat(pu,bcname);
            v = pu;
        }
        if (strcmp(t,"LB") == 0) {
            if (lib) v = lib;        // use library name
        }
        if (strcmp(t,"DS") == 0) {
            if (desc) v = desc;       // use desc
        }
        if (strcmp(t,"SM") == 0) {
            if (sample) v = sample;     // use sample name
        }
        sam_hdr_update(sh, hdr, t, v, NULL);
        if (pu) free(pu);
    }
    free(newtag);
}

/*
 * for each "@RG ID:x" in the header, replace with
 * "@RG IDx#barcode" for each barcode
 *
 * And don't forget to add a @PG header
 */ 
void changeHeader(bc_array_t *barcodeArray, bam_hdr_t *h, char *argv_list)
{
    SAM_hdr *sh = sam_hdr_parse_(h->text, h->l_text);
    char **rgArray = malloc(sizeof(char*) * sh->nrg);
    int nrg = sh->nrg;
    int i, n;

    sam_hdr_add_PG(sh, "bambi", "VN", bambi_version(), "CL", argv_list, NULL);

    // store the RG names
    for (n=0; n < sh->nrg; n++) {
        // store the names and tags as a string <name>:<tag>:<val>:<tag>:<val>...
        // eg 1:PL:Illumina:PU:110608_HS19

        // first pass to determine size of string required
        int sz=strlen(sh->rg[n].name)+1;
        SAM_hdr_tag *rgtag = sh->rg[n].tag;
        while (rgtag) {
            if (strncmp(rgtag->str,"ID:",3)) {  // ignore name
                sz += 3 + strlen(rgtag->str) + 1;
            }
            rgtag = rgtag->next;
        }
        char *entry = malloc(sz+1);

        // second pass to create string
        strcpy(entry,sh->rg[n].name);
        rgtag = sh->rg[n].tag;
        while (rgtag) {
            if (strncmp(rgtag->str,"ID:",3)) {  // ignore name
                strcat(entry,"\t");
                strcat(entry,rgtag->str);
            }
            rgtag = rgtag->next;
        }
        rgArray[n] = entry;
    }

    // delete the old RG lines
    sh = sam_hdr_del(sh, "RG", NULL, NULL);

    // add the new ones
    for (n=0; n<nrg; n++) {
        char *entry = strdup(rgArray[n]);
        addNewRG(sh, entry, "0", NULL, NULL, NULL);
        free(entry);

        // for each tag in barcodeArray
        for (i=1; i < barcodeArray->end; i++) {
            bc_details_t *bcd = barcodeArray->entries[i];

            char *entry = strdup(rgArray[n]);
            addNewRG(sh, entry, bcd->name, bcd->lib, bcd->sample, bcd->desc);
            free(entry);
        }
    }

    for (n=0; n<nrg; n++) {
        free(rgArray[n]);
    }
    free(rgArray);

    free(h->text);
    sam_hdr_rebuild(sh);
    h->text = strdup(sam_hdr_str(sh));
    h->l_text = sam_hdr_length(sh);
    sam_hdr_free(sh);
}

/*
 * Process one BAM record
 */
int processRecord(samFile *input_file, bam_hdr_t *input_header, samFile *output_file, bam_hdr_t *output_header, bc_array_t *barcodeArray, opts_t *opts)
{
    bam1_t* file_read = bam_init1();
    bam1_t* paired_read = bam_init1();
    char *name = NULL;

    int r = sam_read1(input_file, input_header, file_read);
    if (r < 0) {    // end of file
        bam_destroy1(paired_read);
        bam_destroy1(file_read);
        return 1;
    }

    // look for barcode tag
    uint8_t *p = bam_aux_get(file_read,opts->barcode_tag_name);
    if (p) {
        char *seq = bam_aux2Z(p);
        char *newseq = strdup(seq);
        if (opts->convert_low_quality) {
            uint8_t *q = bam_aux_get(file_read,opts->quality_tag_name);
            if (q) {
                char *qual = bam_aux2Z(q);
                free(newseq);
                newseq = checkBarcodeQuality(seq,qual,opts->max_low_quality_to_convert);
            }
        }
        if (strlen(seq) > barcodeArray->tag_len) {
            newseq[barcodeArray->tag_len] = 0;  // truncate seq to barcode length
        }
        name = findBarcodeName(newseq,barcodeArray,opts,!(file_read->core.flag & BAM_FQCFAIL));
        if (!name) name = "0";
        char * newtag = makeNewTag(file_read,"RG",name);
        bam_aux_update_str(file_read,"RG",strlen(newtag)+1, newtag);
        free(newtag);
        if (opts->change_read_name) add_suffix(file_read, name);
        free(newseq);
    }
    r = sam_write1(output_file, output_header, file_read);
    if (r < 0) {
        fprintf(stderr, "Could not write sequence\n");
        return -1;
    }
        
    if (file_read->core.flag & BAM_FPAIRED) {
        r = sam_read1(input_file, input_header, paired_read);
        if (p) {
            char *newtag = makeNewTag(paired_read,"RG",name);
            bam_aux_update_str(paired_read,"RG",strlen(newtag)+1,newtag);
            free(newtag);
        }
        if (opts->change_read_name) add_suffix(paired_read, name);
        r = sam_write1(output_file, output_header, paired_read);
        if (r < 0) {
            fprintf(stderr, "Could not write sequence\n");
            return -1;
        }
    }

    bam_destroy1(paired_read);
    bam_destroy1(file_read);
    return 0;
}
 
/*
 * Main code
 */
static int decode(opts_t* opts)
{
    int retcode = 1;
    samFile *input_file = NULL;
    bam_hdr_t *input_header = NULL;
    samFile *output_file = NULL;
    bam_hdr_t *output_header = NULL;
    htsFormat *in_fmt = NULL;
    htsFormat *out_fmt = NULL;
    char mode[] = "wbC";

    while (1) {
        /*
         * Read the barcode (tags) file 
         */
        bc_array_t *barcodeArray = loadBarcodeFile(opts->barcode_name);
        if (!barcodeArray) break;

        /*
         * Open input file and header
         */
        if (opts->input_fmt) {
            in_fmt = calloc(1,sizeof(htsFormat));
            if (hts_parse_format(in_fmt, opts->input_fmt) < 0) {
                fprintf(stderr,"Unknown input format: %s\n", opts->input_fmt);
                break;
            }
        }
        input_file = hts_open_format(opts->input_name, "rb", in_fmt);
        free(in_fmt);
        if (!input_file) {
            fprintf(stderr, "Could not open input file (%s)\n", opts->input_name);
            break;
        }

        input_header = sam_hdr_read(input_file);
        if (!input_header) {
            fprintf(stderr, "Could not read header for file '%s'\n", opts->input_name);
            break;
        }

        /*
         * Open output file and header
         */
        if (opts->output_fmt) {
            out_fmt = calloc(1,sizeof(htsFormat));
            if (hts_parse_format(out_fmt, opts->output_fmt) < 0) {
                fprintf(stderr,"Unknown output format: %s\n", opts->output_fmt);
                break;
            }
        }
        mode[2] = opts->compression_level ? opts->compression_level : '\0';
        output_file = hts_open_format(opts->output_name, mode, out_fmt);
        free(out_fmt);
        if (!output_file) {
            fprintf(stderr, "Could not open output file (%s)\n", opts->output_name);
            break;
        }

        output_header = bam_hdr_dup(input_header);
        if (!output_header) {
            fprintf(stderr, "Failed to duplicate input header\n");
            break;
        }

        // Change header by adding PG and RG lines
        changeHeader(barcodeArray, output_header, opts->argv_list);

        if (sam_hdr_write(output_file, output_header) != 0) {
            fprintf(stderr, "Could not write output file header\n");
            break;
        }

        /*
         * Process each BAM record, collecting metrics as we go
         */
        while (0 == processRecord(input_file, input_header, output_file, output_header, barcodeArray, opts));

        /*
         * And finally.....the metrics
         */
        if (opts->metrics_name) {
            if (writeMetrics(barcodeArray, opts) != 0) break;
        }
                
        bc_free(barcodeArray);

        retcode = 0;
        break;
    }

    // tidy up after us
    if (input_header) bam_hdr_destroy(input_header);
    if (output_header) bam_hdr_destroy(output_header);
    if (input_file) sam_close(input_file);
    if (output_file) sam_close(output_file);
    
    return retcode;
}

/*
 * called from bambi to perform index decoding
 *
 * Parse the command line arguments, then call the main decode function
 *
 * returns 0 on success, 1 if there was a problem
 */
int main_decode(int argc, char *argv[])
{
    int ret = 1;
    opts_t* opts = parse_args(argc, argv);
    if (opts) {
        ret = decode(opts);
    }
    free_opts(opts);
    return ret;
}