// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Copyright (c) 2010-2015 Illumina, Inc.
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/**
 * Track named regions
 *
 * \file QuantifyRegions.cpp
 * \author Peter Krusche
 * \email pkrusche@illumina.com
 *
 */

#include <boost/filesystem.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include "QuantifyRegions.hh"

#include "helpers/IntervalBuffer.hh"
#include "helpers/BCFHelpers.hh"

#include <map>
#include <unordered_map>

#include "Error.hh"

namespace variant
{
    struct QuantifyRegions::QuantifyRegionsImpl
    {
        std::vector<std::string> names;
        std::unordered_map<std::string, size_t> label_map;
        std::unordered_map<std::string, std::unique_ptr<intervals::IntervalBuffer>> ib;
        std::unordered_map<std::string, std::unique_ptr<intervals::IntervalBuffer>>::iterator current_chr = ib.end();
        std::unordered_map<size_t, size_t> region_sizes;
        int64_t current_pos = -1;
    };

    QuantifyRegions::QuantifyRegions() : _impl(new QuantifyRegionsImpl())
    { }

    QuantifyRegions::~QuantifyRegions()
    { }

    /**
     * Returns true if regions were loaded.
     *
     * Note that this will also return true when an empty bed file was loaded.
     * This is intentional to distinguish the case where we don't have confident
     * regions (everything unknown is a FP) from the one where the confident
     * region file is empty (every FP is unknown).
     */
    bool QuantifyRegions::hasRegions(std::string const & rname) const
    {
        return _impl->label_map.find(rname) != _impl->label_map.cend();
    }

    void QuantifyRegions::load(std::vector<std::string> const &rnames, bool fixchr)
    {
        std::unordered_map<std::string, size_t> label_map;
        for (std::string const &f : rnames)
        {
            std::vector<std::string> v;
            stringutil::split(f, v, ":");
            bool fixed_label = false;

            std::string filename, label = "";

            // in case someone passes a ":"
            if (v.size() == 0)
            {
                error("Invalid region name: %s", f.c_str());
            }

            if (v.size() > 1)
            {
                label = v[0];
                filename = v[1];
                if(label[0] == '=')
                {
                    label = label.substr(1);
                    fixed_label = true;
                }
                if(label == "CONF")
                {
                    fixed_label = true;
                }
            }
            else
            {
                filename = v[0];
                label = boost::filesystem::path(filename).stem().string();
            }

            htsFile *bedfile = NULL;

            if (stringutil::endsWith(filename, ".gz"))
            {
                bedfile = hts_open(filename.c_str(), "rz");
            }
            else
            {
                bedfile = hts_open(filename.c_str(), "r");
            }

            size_t icount = 0;
            kstring_t l;
            l.l = l.m = 0;
            l.s = NULL;
            size_t label_id;
            auto li_it = label_map.find(label);
            if (li_it == label_map.end())
            {
                label_id = _impl->names.size();
                _impl->names.push_back(label);
                label_map[label] = label_id;
            }
            else
            {
                label_id = li_it->second;
            }
            while (hts_getline(bedfile, 2, &l) > 0)
            {
                std::string line(l.s);
                v.clear();
                stringutil::split(line, v, "\t");
                // we want >= 3 columns
                if (v.size() >= 3)
                {
                    if (fixchr)
                    {
                        if(v[0].size() > 0 && (
                            v[0].at(0) == '1' ||
                            v[0].at(0) == '2' ||
                            v[0].at(0) == '3' ||
                            v[0].at(0) == '4' ||
                            v[0].at(0) == '5' ||
                            v[0].at(0) == '6' ||
                            v[0].at(0) == '7' ||
                            v[0].at(0) == '8' ||
                            v[0].at(0) == '9' ||
                            v[0].at(0) == 'X' ||
                            v[0].at(0) == 'Y' ||
                            v[0].at(0) == 'M' ))
                        {
                            v[0] = std::string("chr") + v[0];
                        }
                    }
                    auto chr_it = _impl->ib.find(v[0]);
                    if (chr_it == _impl->ib.end())
                    {
                        chr_it = _impl->ib.emplace(
                            v[0],
                            std::move(
                                std::unique_ptr<intervals::IntervalBuffer>(new intervals::IntervalBuffer()))).first;
                    }
                    // intervals are both zero-based
                    try
                    {

                        int64_t start = (int64_t) std::stoll(v[1]), stop = (int64_t) (std::stoll(v[2]) - 1);
                        if (start > stop)
                        {
                            std::cerr << "[W] ignoring invalid interval in " << filename << " : " << line << "\n";
                            continue;
                        }

                        size_t this_label_id = label_id;
                        if(!fixed_label && v.size() > 3) {
                            const std::string entry_label = label + "_" + v[3];
                            auto li_it2 = label_map.find(entry_label);
                            if (li_it2 == label_map.end())
                            {
                                this_label_id = _impl->names.size();
                                _impl->names.push_back(entry_label);
                                label_map[entry_label] = this_label_id;
                            }
                            else
                            {
                                this_label_id = li_it2->second;
                            }
                        }
                        auto size_it = _impl->region_sizes.find(this_label_id);
                        if(size_it == _impl->region_sizes.end())
                        {
                            _impl->region_sizes[this_label_id] = (unsigned long) (stop - start + 1);
                        }
                        else
                        {
                            size_it->second += (unsigned long) (stop - start + 1);
                        }
                        chr_it->second->addInterval(start, stop, this_label_id);
                        if(this_label_id != label_id)
                        {
                            // also add to total for this bed file
                            size_it = _impl->region_sizes.find(label_id);
                            if(size_it == _impl->region_sizes.end())
                            {
                                _impl->region_sizes[label_id] = (unsigned long) (stop - start + 1);
                            }
                            else
                            {
                                size_it->second += (unsigned long) (stop - start + 1);
                            }
                            chr_it->second->addInterval(start, stop, label_id);
                        }
                        ++icount;
                    }
                    catch (std::invalid_argument const &)
                    {
                        std::cerr << "[W] ignoring invalid interval in " << filename << " : " << line << "\n";
                    }
                    catch (std::out_of_range const &)
                    {
                        std::cerr << "[W] ignoring invalid interval in " << filename << " : " << line << "\n";
                    }
                }
                else if (line != "" && line != "\n")
                {
                    std::cerr << "[W] ignoring mis-formatted input line in " << filename << " : " << line << "\n";
                }
            }
            free(l.s);
            hts_close(bedfile);
            std::cerr << "Added region file '" << filename << "' as '" << label << "' (" << icount << " intervals)" <<
            "\n";
        }
        _impl->label_map = label_map;
    }

    /** add Regions annotation to a record
     *
     * Records must be passed in sorted order.
     *
     */
    void QuantifyRegions::annotate(bcf_hdr_t * hdr, bcf1_t *record)
    {
        std::string chr = bcfhelpers::getChrom(hdr, record);
        int64_t refstart = 0, refend = 0;
        bcfhelpers::getLocation(hdr, record, refstart, refend);

        std::string tag_string = "";
        std::set<std::string> regions;

        auto p_chr = _impl->current_chr;
        if(p_chr == _impl->ib.end() || p_chr->first != chr)
        {
            _impl->current_pos = -1;
            p_chr = _impl->ib.find(chr);
        }

        if(p_chr != _impl->ib.end())
        {
            if(refstart < _impl->current_pos)
            {
                error("Variants out of order at %s:%i", chr.c_str(), refstart);
            }
            for(size_t i = 0; i < _impl->names.size(); ++i)
            {
                if(p_chr->second->hasOverlap(refstart, refend, i))
                {
                    regions.insert(_impl->names[i]);
                }
            }
            if(refstart > 1)
            {
                _impl->current_pos = refstart - 1;
                p_chr->second->advance(refstart-1);
            }
        }
        // regions set is sorted, make sure Regions is sorted also
        for(auto const & r : regions)
        {
            if(!tag_string.empty())
            {
                tag_string += ",";
            }
            tag_string += r;
        }
        if(!tag_string.empty())
        {
            bcf_update_info_string(hdr, record, "Regions", tag_string.c_str());
        }
        else
        {
            bcf_update_info_string(hdr, record, "Regions", nullptr);
        }
    }

    /**
     * Get total region sizes in NT
     * @param region_name
     * @return  the region size
     */
    size_t QuantifyRegions::getRegionSize(std::string const & region_name) const
    {
        auto label_it = _impl->label_map.find(region_name);
        if(label_it == _impl->label_map.cend())
        {
            return 0;
        }
        auto size_it = _impl->region_sizes.find(label_it->second);
        if(size_it == _impl->region_sizes.cend())
        {
            return 0;
        }
        return size_it->second;
    }
}


