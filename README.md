McCortex: Population De Novo Assembly and Variant Calling
===============================================

Multi-sample de novo assembly and variant calling using de bruijn graphs.
Variant calling with and without a reference genome. Between closely related
samples or highly diverged ones. From bacterial to mammalian genomes. Minimal
configuration. And it's free.

Isaac Turner's experimental rewrite of cortex_var, to handle larger populations
with better genome assembly. PhD supervisor: Prof Gil McVean. Collaborators: Zam Iqbal, Kiran Garimella. Based at the Wellcome Trust Centre for Human Genetics, University of Oxford.

*Note: Currently under development.* Expect bugs, fixes and vague documentation until we hit our first release in the next month. Feel free to try out McCortex and watch this space for the release. An announcement will be made on the [cortex mailing list](https://groups.google.com/forum/#!forum/cortex_var).

1 September 2014

[![Build Status](https://travis-ci.org/mcveanlab/mccortex.svg)](https://travis-ci.org/mcveanlab/mccortex)[![Coverity Scan Build Status](https://scan.coverity.com/projects/2329/badge.svg)](https://scan.coverity.com/projects/2329)

Build
-----

Compiles with clang and gcc. Tested on Mac OS X and linux. Requires zlib.
The first compile will take a while since the libraries in libs/ need to be
downloaded and compiled.

To compile for a maximum kmer size of 31:

    make

to compile for a maximum kmer size of 63:

    make MAXK=63

Executables appear in the `bin/` directory. To update the libraries included:

    cd libs && make

Commands
--------

    usage: ctx31 <command> [options] <args>
    version: ctx=XXXX zlib=1.2.5 htslib=0.2.0-rc10-8-g4b024e1 ASSERTS=ON CHECKS=ON k=3..31

    Commands:   breakpoints  use a trusted assembled genome to call large events
                bubbles      find bubbles in graph which are potential variants
                build        construct cortex graph from FASTA/FASTQ/BAM
                check        load and check graph (.ctx) and path (.ctp) files
                clean        clean errors from a graph
                contigs      assemble contigs for a sample
                correct      error correct reads
                coverage     print contig coverage
                index        index a sorted cortex graph file
                inferedges   infer graph edges between kmers before calling `thread`
                join         combine graphs, filter graph intersections
                pjoin        merge path files (.ctp)
                pview        text view of a cortex path file (.ctp)
                reads        filter reads against a graph
                rmsubstr     reduce set of strings to remove substrings
                sort         sort the kmers in a graph file
                subgraph     filter a subgraph using seed kmers
                supernodes   pull out supernodes
                thread       thread reads through cleaned graph
                uniqkmers    generate random unique kmers
                view         text view of a cortex graph file (.ctx)

      Type a command with no arguments to see help.

    Common Options:
      -h, --help            Help message
      -q, --quiet           Silence status output normally printed to STDERR
      -f, --force           Overwrite output files if they already exist
      -m, --memory <M>      Memory e.g. 1GB [default: 1GB]
      -n, --nkmers <H>      Hash entries [default: 4M, ~4 million]
      -t, --threads <T>     Limit on proccessing threads [default: 2]
      -o, --out <file>      Output file
      -p, --paths <in.ctp>  Assembly file to load (can specify multiple times)

Getting Helps
-------------

Type a command with no arguments to see usage. The following may also be useful:
* [HipChat](http://www.hipchat.com/gbF6Zf4k3)
* [wiki](https://github.com/mcveanlab/mccortex/wiki)
* [website](http://mcveanlab.github.io/mccortex)
* [mailing list](https://groups.google.com/forum/#!forum/cortex_var)
* Report a [bug](https://github.com/mcveanlab/mccortex/issues)
* Email me: Isaac Turner <turner.isaac@gmail.com>

Code And Contributing
---------------------

Issues can be submitted on github. Pull requests welcome. Please add your name
to the AUTHORS file. Code should compile on mac/linux with clang/gcc without errors or warnings.

More on the [wiki](https://github.com/mcveanlab/mccortex/wiki/Contributing)

Static analysis can be run with [cppcheck](http://cppcheck.sourceforge.net):

    cppcheck src

or with [clang](http://clang-analyzer.llvm.org):

    rm -rf bin/ctx31
    scan-build make RECOMPILE=1

Occasionally we also run Coverity Scan. This is done by pushing to the `coverity_scan` branch on github, which triggers Travis CI to upload the latest code to Coverity.

[![Coverity Scan Build Status](https://scan.coverity.com/projects/2329/badge.svg)](https://scan.coverity.com/projects/2329)

License: MIT
------------

Bundled libraries may have different licenses:
* [cJSON](http://http://sourceforge.net/projects/cjson/) (MIT)
* [CityHash](https://code.google.com/p/cityhash/) (MIT)
* [lookup3](http://burtleburtle.net/bob/c/lookup3.c) (Public Domain)
* [htslib](https://github.com/samtools/htslib) (MIT)
* [seq_file](https://github.com/noporpoise/seq_file) (Public Domain)
* [string_buffer](https://github.com/noporpoise/string_buffer) (Public Domain)
* [BitArray](https://github.com/noporpoise/BitArray) (Public Domain)
* [msg-pool](https://github.com/noporpoise/msg-pool) (Public Domain)
* [seq-align](https://github.com/noporpoise/seq-align) (Public Domain)
* [madcrowlib](https://github.com/noporpoise/madcrowlib) (MIT)

Used in testing:
* [bioinf-perl](https://github.com/noporpoise/bioinf-perl) (Public Domain)
* [samtools](https://github.com/samtools/samtools) (MIT)
* [bcftools](https://github.com/samtools/bcftools) (MIT)
* [vcflib](https://github.com/ekg/vcflib) (MIT)

Citing
------

'Cortex with low memory and read threading' is currently unpublished.  Please
cite previous cortex_var papers:

* De novo assembly and genotyping of variants using colored de Bruijn graphs,
Iqbal(*), Caccamo(*), Turner, Flicek, McVean (Nature Genetics) (2012)
(doi:10.1038/ng.1028)
* High-throughput microbial population genomics using the Cortex variation assembler,
Iqbal, Turner, McVean (Bioinformatics) (Nov 2012)
(doi:10.1093/bioinformatics/bts673)
