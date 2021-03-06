/*
 * Copyright 2016 Jacopo Urbani
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
**/

#ifndef COMPRESSOR_H_
#define COMPRESSOR_H_

#include <kognac/filereader.h>
#include <kognac/hashtable.h>

#include <kognac/lz4io.h>
#include <kognac/hashfunctions.h>
#include <kognac/hashmap.h>
#include <kognac/factory.h>

#ifdef COUNTSKETCH
#include <kognac/CountSketch.h>
#endif

#include <kognac/MisraGries.h>

#include <sparsehash/dense_hash_map>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include <queue>
#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <set>
#include <assert.h>

using namespace std;

#define IDX_SPO 0
#define IDX_OPS 1
#define IDX_POS 2
#define IDX_SOP 3
#define IDX_OSP 4
#define IDX_PSO 5

class SchemaExtractor;
struct ParamsExtractCommonTermProcedure {
    string inputFile;
    Hashtable **tables;
    GStringToNumberMap *map;
    int dictPartitions;
    string *dictFileName;
    int maxMapSize;
    int idProcess;
    int parallelProcesses;
    string *singleTerms;
    int thresholdForUncommon;
    bool copyHashes;
};

struct ParamsNewCompressProcedure {
    string *permDirs;
    int nperms;
    int signaturePerms;
    string prefixOutputFile;
    int part;
    int parallelProcesses;
    int itrN;
    string *inNames;
    ByteArrayToNumberMap *commonMap;
    CompressedByteArrayToNumberMap *map;
    string *uncommonTermsFile;
};

struct ParamsUncompressTriples {
    vector<FileInfo> files;
    Hashtable *table1;
    Hashtable *table2;
    Hashtable *table3;
    string outFile;
    SchemaExtractor *extractor;
    long *distinctValues;
    std::vector<string> *resultsMGS;
    size_t sizeHeap;
};

struct TriplePair {
    long tripleIdAndPosition;
    long term;

    void readFrom(LZ4Reader *reader) {
        tripleIdAndPosition = reader->parseLong();
        term = reader->parseLong();
    }

    void writeTo(LZ4Writer *writer) {
        writer->writeLong(tripleIdAndPosition);
        writer->writeLong(term);
    }

    bool greater(const TriplePair &t1) const {
        return tripleIdAndPosition > t1.tripleIdAndPosition;
    }

    static bool sLess(const TriplePair &t1, const TriplePair &t2) {
        return t1.tripleIdAndPosition < t2.tripleIdAndPosition;
    }

};

struct AnnotatedTerm {
    const char *term;
    int size;
    long tripleIdAndPosition;

    bool useHashes;
    long hashT1, hashT2;

    AnnotatedTerm() {
        term = NULL;
        size = 0;
        tripleIdAndPosition = -1;
        useHashes = false;
    }

    static bool sLess(const AnnotatedTerm &t1, const AnnotatedTerm &t2) {
        int l1 = t1.size - 2;
        int l2 = t2.size - 2;
        int ret = memcmp(t1.term + 2, t2.term + 2, min(l1, l2));
        if (ret == 0) {
            return (l1 - l2) < 0;
        } else {
            return ret < 0;
        }
    }

    bool less(const AnnotatedTerm &t1) const {
        return sLess(*this, t1);
    }

    bool greater(const AnnotatedTerm &t1) const {
        int l1 = size - 2;
        int l2 = t1.size - 2;
        int ret = memcmp(term + 2, t1.term + 2, min(l1, l2));
        if (ret == 0) {
            return (l1 - l2) > 0;
        } else {
            return ret > 0;
        }
    }

    void readFrom(LZ4Reader *reader) {
        term = reader->parseString(size);

        char b = reader->parseByte();
        if (b >> 1 != 0) {
            tripleIdAndPosition = reader->parseLong();
            if (b & 1) {
                useHashes = true;
                hashT1 = reader->parseLong();
                hashT2 = reader->parseLong();
            } else {
                useHashes = false;
            }
        } else {
            tripleIdAndPosition = -1;
            useHashes = false;
        }
    }

    void writeTo(LZ4Writer *writer) {
        writer->writeString(term, size);

        if (useHashes) {
            writer->writeByte(3);
            writer->writeLong(tripleIdAndPosition);
            writer->writeLong(hashT1);
            writer->writeLong(hashT2);
        } else {
            if (tripleIdAndPosition == -1) {
                writer->writeByte(0);
            } else {
                writer->writeByte(2);
                writer->writeLong(tripleIdAndPosition);
            }

        }
    }

    bool equals(const char *el) {
        int l = Utils::decode_short(el);
        if (l == size - 2) {
            return memcmp(term + 2, el + 2, l) == 0;
        }
        return false;
    }

    bool equals(const char *el, int size) {
        if (size == this->size) {
            return memcmp(term + 2, el + 2, size - 2) == 0;
        }
        return false;
    }
};

struct priorityQueueOrder {
    bool operator()(const std::pair<string, long> &lhs,
                    const std::pair<string, long>&rhs) const {
        return lhs.second > rhs.second;
    }
};

class StringCollection;
class LRUSet;

class Compressor {

private:
    const string input;
    const string kbPath;
    long totalCount;
    long nTerms;
    std::shared_ptr<Hashtable> table1;
    std::shared_ptr<Hashtable> table2;
    std::shared_ptr<Hashtable> table3;

    void do_sample(const int dictPartitions, const int sampleArg,
                   const int sampleArg2,
                   const int maxReadingThreads, bool copyHashes,
                   const int parallelProcesses,
                   SchemaExtractor *extractors, vector<FileInfo> *files,
                   GStringToNumberMap *commonTermsMaps);

    void do_mcgs();

    void do_countmin(const int dictPartitions, const int sampleArg,
                     const int parallelProcesses, const int maxReadingThreads,
                     const bool copyHashes, SchemaExtractor *extractors,
                     vector<FileInfo> *files,
                     GStringToNumberMap *commonTermsMaps, bool usemisgra);

    void do_countmin_secondpass(const int dictPartitions,
                                const int sampleArg,
                                const int parallelProcesses,
                                bool copyHashes,
                                const unsigned int sizeHashTable,
                                Hashtable **tables1,
                                Hashtable **tables2,
                                Hashtable **tables3,
                                long *distinctValues,
                                GStringToNumberMap *commonTermsMaps);

    unsigned int getThresholdForUncommon(
        const int parallelProcesses,
        const int sizeHashTable,
        const int sampleArg,
        long *distinctValues,
        Hashtable **tables1,
        Hashtable **tables2,
        Hashtable **tables3);


    static std::vector<string> getPartitionBoundaries(const string kbdir,
            const int partitions);

    static void rangePartitionFiles(int nthreads, vector<string> *inputFiles,
                                    std::vector<string> &outputFiles,
                                    const std::vector<string> &boundaries);

    static void sortRangePartitionedTuples(const string inputPrefix,
                                           const string outputFile,
                                           const std::vector<string> *boundaries);

    static void sortPartitionsAndAssignCounters(std::vector<string> &inputFiles,
            string dictfile, string outputfile, int partitions,
            long &counter, int parallelProcesses);

    static void sortPartition(std::vector<string> *inputFiles,
                              string dictfile, string outputfile, int part,
                              uint64_t *counter, long maxMem);

    static void assignCountersAndPartByTripleID(long startCounter,
            string infile, string outfile, int parallelProcesses);

    //static void sampleTuples(string input, std::vector<string> *output);

protected:
    static bool isSplittable(string path);

    string getKBPath() {
        return kbPath;
    }

    void sampleTerm(const char *term, int sizeTerm, int sampleArg,
                    int dictPartitions, GStringToNumberMap * map/*,
                    LRUSet *duplicateCache, LZ4Writer **dictFile*/);

    void uncompressTriples(ParamsUncompressTriples params);

#ifdef COUNTSKETCH
    void uncompressTriplesForMGCS(vector<FileInfo> &files, MG * heap, CountSketch * cs, string outFile,
                                  SchemaExtractor * extractor, long * distinctValues);

    void extractTermsForMGCS(ParamsExtractCommonTermProcedure params, const set<string>& freq,
                             const CountSketch * cs);

    void extractTermForMGCS(const char *term, const int sizeTerm, unsigned long & countFreq, unsigned long & countInfrequent,
                            const int dictPartition, const bool copyHashes, const long tripleId, const int pos,
                            char **prevEntries, int *sPrevEntries, LZ4Writer **dictFile, LZ4Writer **udictFile,
                            const set<string>& freq, const CountSketch * cs);
#endif

    void uncompressAndSampleTriples(vector<FileInfo> &files, string outFile,
                                    string * dictFileName, int dictPartitions,
                                    int sampleArg,
                                    GStringToNumberMap * map,
                                    SchemaExtractor * extractor);

    void extractUncommonTerm(const char *term, const int sizeTerm,
                             ByteArrayToNumberMap * map,
                             LZ4Writer **udictFile,
                             const long tripleId,
                             const int pos,
                             const int dictPartitions,
                             const bool copyHashes,
                             char **prevEntries, int *sPrevEntries);

    void extractCommonTerm(const char* term, const int sizeTerm, long & countFrequent,
                           const long thresholdForUncommon, Hashtable * table1,
                           Hashtable * table2, Hashtable * table3, const int dictPartitions,
                           long & minValueToBeAdded,
                           const long maxMapSize, GStringToNumberMap * map,
                           std::priority_queue<std::pair<string, long>,
                           std::vector<std::pair<string, long> >, priorityQueueOrder> &queue);

    void extractCommonTerms(ParamsExtractCommonTermProcedure params);

    void extractUncommonTerms(const int dictPartitions, string inputFile,
                              const bool copyHashes, const int idProcess,
                              const int parallelProcesses,
                              string * udictFileName,
                              const bool splitUncommonByHash);

    void mergeCommonTermsMaps(ByteArrayToNumberMap * finalMap,
                              GStringToNumberMap * maps, int nmaps);

    void mergeNotPopularEntries(vector<string> *inputFiles,
                                string globalDictOutput, string outputFile1, string outputFile2,
                                long * startCounter, int increment, int parallelProcesses);

    void assignNumbersToCommonTermsMap(ByteArrayToNumberMap * finalMap,
                                       long * counters, LZ4Writer **writers,
                                       LZ4Writer **invWriters, int ndictionaries,
                                       bool preserveMapping);

    void newCompressTriples(ParamsNewCompressProcedure params);

    bool areFilesToCompress(int parallelProcesses, string * tmpFileNames);

    static void sortAndDumpToFile(vector<AnnotatedTerm> &vector, string outputFile,
                                  bool removeDuplicates);

    void sortAndDumpToFile2(vector<TriplePair> &pairs, string outputFile);

    void compressTriples(const int parallelProcesses, const int ndicts,
                         string * permDirs, int nperms, int signaturePerms, vector<string> &notSoUncommonFiles,
                         vector<string> &finalUncommonFiles, string * tmpFileNames,
                         StringCollection * poolForMap, ByteArrayToNumberMap * finalMap);

    void sortFilesByTripleSource(string kbPath, const int parallelProcesses, const int ndicts,
                                 vector<string> uncommonFiles, vector<string> &finalUncommonFiles);

    void sortByTripleID(vector<string> *inputFiles, string outputFile,
                        const long maxMemory);

    void immemorysort(string **inputFiles, int parallelProcesses, string outputFile, int *noutputFiles,
                      bool removeDuplicates,
                      const long maxSizeToSort, bool sample);

    void inmemorysort_seq(const string inputFile,
                          int idx,
                          const int incrIdx,
                          const long maxMemPerThread,
                          bool removeDuplicates,
                          string outputFile,
                          bool sample);

    void sortDictionaryEntriesByText(string **input, const int ndicts,
                                     const int parallelProcesses,
                                     string * prefixOutputFiles,
                                     int *noutputfiles,
                                     ByteArrayToNumberMap * map,
                                     bool filterDuplicates,
                                     bool sample);

    static unsigned long calculateSizeHashmapCompression();

    static unsigned long calculateMaxEntriesHashmapCompression();

public:
    Compressor(string input, string kbPath);

    static void addPermutation(const int permutation, int &output);

    static void parsePermutationSignature(int signature, int *output);

    unsigned long getEstimatedFrequency(const string & el) const;

    static vector<FileInfo> *splitInputInChunks(const string & input, int nchunks);

    void parse(int dictPartitions, int sampleMethod, int sampleArg, int sampleArg2,
               int parallelProcesses, int maxReadingThreads, bool copyHashes,
               SchemaExtractor * extractor, const bool splitUncommonByHash) {
        parse(dictPartitions, sampleMethod, sampleArg, sampleArg2,
              parallelProcesses, maxReadingThreads, copyHashes, extractor,
              splitUncommonByHash, false);
    }

    void parse(int dictPartitions, int sampleMethod, int sampleArg, int sampleArg2,
               int parallelProcesses, int maxReadingThreads, bool copyHashes,
               SchemaExtractor * extractor, const bool splitUncommonByHash,
               bool onlySample);

    virtual void compress(string * permDirs, int nperms, int signaturePerms,
                          string * dictionaries, int ndicts, int parallelProcesses);

    string **dictFileNames;
    string **uncommonDictFileNames;
    string *tmpFileNames;
    StringCollection *poolForMap;
    ByteArrayToNumberMap *finalMap;

    long getTotalCount() {
        return totalCount;
    }

    long getEstimateNumberTerms() {
        return nTerms;
    }

    void cleanup() {
        table1 = std::shared_ptr<Hashtable>();
        table2 = std::shared_ptr<Hashtable>();
        table3 = std::shared_ptr<Hashtable>();
    }

    static std::vector<string> getAllDictFiles(string prefixDict);

    ~Compressor();
};

#endif /* COMPRESSOR_H_ */
