// This file is part of PLINK 2.00, copyright (C) 2005-2018 Shaun Purcell,
// Christopher Chang.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "plink2_adjust.h"
#include "plink2_compress_stream.h"
#include "plink2_stats.h"

#ifdef __cplusplus
namespace plink2 {
#endif

void init_adjust(adjust_info_t* adjust_info_ptr, adjust_file_info_t* adjust_file_info_ptr) {
  adjust_info_ptr->flags = kfAdjust0;
  adjust_info_ptr->lambda = 0.0;
  adjust_file_info_ptr->base.flags = kfAdjust0;
  adjust_file_info_ptr->base.lambda = 0.0;
  adjust_file_info_ptr->fname = nullptr;
  adjust_file_info_ptr->test_name = nullptr;
  adjust_file_info_ptr->chr_field = nullptr;
  adjust_file_info_ptr->pos_field = nullptr;
  adjust_file_info_ptr->id_field = nullptr;
  adjust_file_info_ptr->ref_field = nullptr;
  adjust_file_info_ptr->alt_field = nullptr;
  adjust_file_info_ptr->p_field = nullptr;
}

void cleanup_adjust(adjust_file_info_t* adjust_file_info_ptr) {
  free_cond(adjust_file_info_ptr->alt_field);
  free_cond(adjust_file_info_ptr->chr_field);
  if (adjust_file_info_ptr->fname) {
    free(adjust_file_info_ptr->fname);
    free_cond(adjust_file_info_ptr->fname);
    free_cond(adjust_file_info_ptr->pos_field);
    free_cond(adjust_file_info_ptr->id_field);
    free_cond(adjust_file_info_ptr->ref_field);
    free_cond(adjust_file_info_ptr->test_field);
    free_cond(adjust_file_info_ptr->p_field);
  }
}

typedef struct adj_assoc_result_struct {
  double chisq;
  double pval;
  uint32_t variant_uidx;
#ifdef __cplusplus
  bool operator<(const struct adj_assoc_result_struct& rhs) const {
    // avoids p-value underflow issue, for what it's worth
    return chisq > rhs.chisq;
  }
#endif
} adj_assoc_result_t;

static inline void adjust_print(const char* output_min_p_str, double pval, double output_min_p, uint32_t output_min_p_slen, uint32_t is_log10, char** bufpp) {
  **bufpp = '\t';
  *bufpp += 1;
  if (pval <= output_min_p) {
    *bufpp = memcpya(*bufpp, output_min_p_str, output_min_p_slen);
  } else {
    if (is_log10) {
      pval = -log10(pval);
    }
    *bufpp = dtoa_g(pval, *bufpp);
  }
}

pglerr_t multcomp(const uintptr_t* variant_include, const chr_info_t* cip, const char* const* chr_ids, const uint32_t* variant_bps, const char* const* variant_ids, const uintptr_t* variant_allele_idxs, const char* const* allele_storage, const adjust_info_t* adjust_info_ptr, const double* pvals, const double* chisqs, uint32_t orig_variant_ct, uint32_t max_allele_slen, double pfilter, double output_min_p, uint32_t skip_gc, uint32_t max_thread_ct, char* outname, char* outname_end) {
  unsigned char* bigstack_mark = g_bigstack_base;
  char* cswritep = nullptr;
  compress_stream_state_t css;
  pglerr_t reterr = kPglRetSuccess;
  cswrite_init_null(&css);
  {
    adj_assoc_result_t* sortbuf = (adj_assoc_result_t*)bigstack_alloc(orig_variant_ct * sizeof(adj_assoc_result_t));
    if (!sortbuf) {
      goto multcomp_ret_NOMEM;
    }
    uint32_t valid_variant_ct = 0;
    if (chisqs) {
      uint32_t variant_uidx = 0;
      if (pvals) {
        for (uint32_t vidx = 0; vidx < orig_variant_ct; ++vidx, ++variant_uidx) {
          next_set_unsafe_ck(variant_include, &variant_uidx);
          const double cur_chisq = chisqs[vidx];
          if (cur_chisq >= 0.0) {
            sortbuf[valid_variant_ct].chisq = cur_chisq;
            sortbuf[valid_variant_ct].pval = pvals[vidx];
            sortbuf[valid_variant_ct].variant_uidx = variant_uidx;
            ++valid_variant_ct;
          }
        }
      } else {
        for (uint32_t vidx = 0; vidx < orig_variant_ct; ++vidx, ++variant_uidx) {
          next_set_unsafe_ck(variant_include, &variant_uidx);
          const double cur_chisq = chisqs[vidx];
          if (cur_chisq >= 0.0) {
            sortbuf[valid_variant_ct].chisq = cur_chisq;
            sortbuf[valid_variant_ct].pval = chiprob_p(cur_chisq, 1);
            sortbuf[valid_variant_ct].variant_uidx = variant_uidx;
            ++valid_variant_ct;
          }
        }
      }
    } else {
      uint32_t variant_uidx = 0;
      for (uint32_t vidx = 0; vidx < orig_variant_ct; ++vidx, ++variant_uidx) {
        next_set_unsafe_ck(variant_include, &variant_uidx);
        const double cur_pval = pvals[vidx];
        if (cur_pval >= 0.0) {
          sortbuf[valid_variant_ct].chisq = (cur_pval == 0.0)? kMaxInverseChiprob1df : inverse_chiprob(cur_pval, 1);
          sortbuf[valid_variant_ct].pval = cur_pval;
          sortbuf[valid_variant_ct].variant_uidx = variant_uidx;
          ++valid_variant_ct;
        }
      }
    }
    if (!valid_variant_ct) {
      logprint("Zero valid tests; --adjust skipped.\n");
      goto multcomp_ret_1;
    }
    bigstack_shrink_top(sortbuf, valid_variant_ct * sizeof(adj_assoc_result_t));

    const uintptr_t overflow_buf_size = kCompressStreamBlock + 2 * kMaxIdSlen + 256 + 2 * max_allele_slen;
    const adjust_flags_t flags = adjust_info_ptr->flags;
    const uint32_t output_zst = flags & kfAdjustZs;
    strcpy(outname_end, output_zst? ".adjusted.zst" : ".adjusted");
    reterr = cswrite_init2(outname, 0, output_zst, max_thread_ct, overflow_buf_size, &css, &cswritep);
    if (reterr) {
      goto multcomp_ret_1;
    }
    *cswritep++ = '#';
    const uint32_t chr_col = flags & kfAdjustColChrom;
    if (chr_col) {
      cswritep = strcpya(cswritep, "CHROM\t");
    }
    if (flags & kfAdjustColPos) {
      cswritep = strcpya(cswritep, "POS\t");
    } else {
      variant_bps = nullptr;
    }
    cswritep = strcpya(cswritep, "ID");
    const uint32_t ref_col = flags & kfAdjustColRef;
    if (ref_col) {
      cswritep = strcpya(cswritep, "\tREF");
    }
    const uint32_t alt1_col = flags & kfAdjustColAlt1;
    if (alt1_col) {
      cswritep = strcpya(cswritep, "\tALT1");
    }
    const uint32_t alt_col = flags & kfAdjustColAlt;
    if (alt_col) {
      cswritep = strcpya(cswritep, "\tALT");
    }
    const uint32_t unadj_col = flags & kfAdjustColUnadj;
    if (unadj_col) {
      cswritep = strcpya(cswritep, "\tUNADJ");
    }
    const uint32_t gc_col = (flags & kfAdjustColGc) && (!skip_gc);
    if (gc_col) {
      cswritep = strcpya(cswritep, "\tGC");
    }
    const uint32_t qq_col = flags & kfAdjustColQq;
    if (qq_col) {
      cswritep = strcpya(cswritep, "\tQQ");
    }
    const uint32_t bonf_col = flags & kfAdjustColBonf;
    if (bonf_col) {
      cswritep = strcpya(cswritep, "\tBONF");
    }
    const uint32_t holm_col = flags & kfAdjustColHolm;
    if (holm_col) {
      cswritep = strcpya(cswritep, "\tHOLM");
    }
    const uint32_t sidakss_col = flags & kfAdjustColSidakss;
    if (sidakss_col) {
      cswritep = strcpya(cswritep, "\tSIDAK_SS");
    }
    const uint32_t sidaksd_col = flags & kfAdjustColSidaksd;
    if (sidaksd_col) {
      cswritep = strcpya(cswritep, "\tSIDAK_SD");
    }
    const uint32_t fdrbh_col = flags & kfAdjustColFdrbh;
    if (fdrbh_col) {
      cswritep = strcpya(cswritep, "\tFDR_BH");
    }
    double* pv_by = nullptr;
    if (flags & kfAdjustColFdrby) {
      if (bigstack_alloc_d(valid_variant_ct, &pv_by)) {
        goto multcomp_ret_NOMEM;
      }
      cswritep = strcpya(cswritep, "\tFDR_BY");
    }
    append_binary_eoln(&cswritep);

    // reverse-order calculations
    double* pv_bh;
    double* pv_gc;
    double* unadj_sorted_pvals;
    if (bigstack_alloc_d(valid_variant_ct, &pv_bh) ||
        bigstack_alloc_d(valid_variant_ct, &pv_gc) ||
        bigstack_alloc_d(valid_variant_ct, &unadj_sorted_pvals)) {
      goto multcomp_ret_NOMEM;
    }

#ifdef __cplusplus
    std::sort(sortbuf, &(sortbuf[valid_variant_ct]));
#else
    qsort(sortbuf, valid_variant_ct, sizeof(adj_assoc_result_t), double_cmp_decr);
#endif

    double lambda_recip = 1.0;
    if (!skip_gc) {
      if (adjust_info_ptr->lambda != 0.0) {
        lambda_recip = 1.0 / adjust_info_ptr->lambda;
      } else {
        const uint32_t valid_variant_ct_d2 = valid_variant_ct / 2;
        double lambda = sortbuf[valid_variant_ct_d2].chisq;
        if (!(valid_variant_ct % 2)) {
          lambda = (lambda + sortbuf[valid_variant_ct_d2 - 1].chisq) * 0.5;
        }
        lambda = lambda / 0.456;
        if (lambda < 1.0) {
          lambda = 1.0;
        }
        LOGPRINTF("--adjust: Genomic inflation est. lambda (based on median chisq) = %g.\n", lambda);
        lambda_recip = 1.0 / lambda;
      }
    }
    double* sorted_pvals = unadj_sorted_pvals;
    for (uint32_t vidx = 0; vidx < valid_variant_ct; ++vidx) {
      pv_gc[vidx] = chiprob_p(sortbuf[vidx].chisq * lambda_recip, 1);
      unadj_sorted_pvals[vidx] = sortbuf[vidx].pval;
    }
    if ((flags & kfAdjustGc) && (!skip_gc)) {
      sorted_pvals = pv_gc;
    }

    const uint32_t valid_variant_ct_m1 = valid_variant_ct - 1;
    const double valid_variant_ctd = (double)((int32_t)valid_variant_ct);
    double bh_pval_min = sorted_pvals[valid_variant_ct_m1];
    pv_bh[valid_variant_ct_m1] = bh_pval_min;
    double harmonic_sum = 1.0;
    for (uint32_t vidx = valid_variant_ct_m1; vidx; --vidx) {
      const double harmonic_term = valid_variant_ctd / ((double)((int32_t)vidx));
      harmonic_sum += harmonic_term;
      const double bh_pval = harmonic_term * sorted_pvals[vidx - 1];
      if (bh_pval_min > bh_pval) {
        bh_pval_min = bh_pval;
      }
      pv_bh[vidx - 1] = bh_pval_min;
    }

    const double valid_variant_ct_recip = 1.0 / valid_variant_ctd;
    if (pv_by) {
      double by_pval_min = harmonic_sum * valid_variant_ct_recip * sorted_pvals[valid_variant_ct_m1];
      if (by_pval_min > 1.0) {
        by_pval_min = 1.0;
      }
      pv_by[valid_variant_ct_m1] = by_pval_min;
      for (uint32_t vidx = valid_variant_ct_m1; vidx; --vidx) {
        double by_pval = (harmonic_sum / ((double)((int32_t)vidx))) * sorted_pvals[vidx - 1];
        if (by_pval_min > by_pval) {
          by_pval_min = by_pval;
        }
        pv_by[vidx - 1] = by_pval_min;
      }
    }

    const uint32_t is_log10 = flags & kfAdjustLog10;
    char output_min_p_buf[16];
    uint32_t output_min_p_slen;
    if (!is_log10) {
      char* str_end = dtoa_g(output_min_p, output_min_p_buf);
      output_min_p_slen = (uintptr_t)(str_end - output_min_p_buf);
    } else if (output_min_p > 0.0) {
      char* str_end = dtoa_g(-log10(output_min_p), output_min_p_buf);
      output_min_p_slen = (uintptr_t)(str_end - output_min_p_buf);
    } else {
      memcpyl3(output_min_p_buf, "inf");
      output_min_p_slen = 3;
    }
    double pv_sidak_sd = 0.0;
    double pv_holm = 0.0;
    uint32_t cur_allele_ct = 2;
    uint32_t vidx = 0;
    for (; vidx < valid_variant_ct; ++vidx) {
      double pval = sorted_pvals[vidx];
      if (pval > pfilter) {
        break;
      }
      const uint32_t variant_uidx = sortbuf[vidx].variant_uidx;
      if (chr_col) {
        if (cip) {
          cswritep = chr_name_write(cip, get_variant_chr(cip, variant_uidx), cswritep);
        } else {
          cswritep = strcpya(cswritep, chr_ids[variant_uidx]);
        }
        *cswritep++ = '\t';
      }
      if (variant_bps) {
        cswritep = uint32toa_x(variant_bps[variant_uidx], '\t', cswritep);
      }
      cswritep = strcpya(cswritep, variant_ids[variant_uidx]);
      uintptr_t variant_allele_idx_base = variant_uidx * 2;
      if (variant_allele_idxs) {
        variant_allele_idx_base = variant_allele_idxs[variant_uidx];
        cur_allele_ct = variant_allele_idxs[variant_uidx + 1] - variant_allele_idx_base;
      }
      const char* const* cur_alleles = &(allele_storage[variant_allele_idx_base]);
      if (ref_col) {
        *cswritep++ = '\t';
        cswritep = strcpya(cswritep, cur_alleles[0]);
      }
      if (alt1_col) {
        *cswritep++ = '\t';
        cswritep = strcpya(cswritep, cur_alleles[1]);
      }
      if (alt_col) {
        *cswritep++ = '\t';
        for (uint32_t allele_idx = 1; allele_idx < cur_allele_ct; ++allele_idx) {
          if (cswrite(&css, &cswritep)) {
            goto multcomp_ret_WRITE_FAIL;
          }
          cswritep = strcpyax(cswritep, cur_alleles[allele_idx], ',');
        }
        --cswritep;
      }
      if (unadj_col) {
        adjust_print(output_min_p_buf, unadj_sorted_pvals[vidx], output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (gc_col) {
        adjust_print(output_min_p_buf, pv_gc[vidx], output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (qq_col) {
        *cswritep++ = '\t';
        cswritep = dtoa_g((((double)((int32_t)vidx)) + 0.5) * valid_variant_ct_recip, cswritep);
      }
      if (bonf_col) {
        const double bonf_pval = MINV(pval * valid_variant_ctd, 1.0);
        adjust_print(output_min_p_buf, bonf_pval, output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (holm_col) {
        if (pv_holm < 1.0) {
          const double pv_holm_new = (double)((int32_t)(valid_variant_ct - vidx)) * pval;
          if (pv_holm_new > 1.0) {
            pv_holm = 1.0;
          } else if (pv_holm < pv_holm_new) {
            pv_holm = pv_holm_new;
          }
        }
        adjust_print(output_min_p_buf, pv_holm, output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (sidakss_col) {
        // avoid catastrophic cancellation for small p-values
        // 1 - (1-p)^c = 1 - e^{c log(1-p)}
        // 2^{-7} threshold is arbitrary
        double pv_sidak_ss;
        if (pval >= 0.0078125) {
          pv_sidak_ss = 1 - pow(1 - pval, valid_variant_ctd);
        } else {
          pv_sidak_ss = 1 - exp(valid_variant_ctd * log1p(-pval));
        }
        adjust_print(output_min_p_buf, pv_sidak_ss, output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (sidaksd_col) {
        double pv_sidak_sd_new;
        if (pval >= 0.0078125) {
          pv_sidak_sd_new = 1 - pow(1 - pval, valid_variant_ctd - ((double)((int32_t)vidx)));
        } else {
          const double cur_exp = valid_variant_ctd - (double)((int32_t)vidx);
          pv_sidak_sd_new = 1 - exp(cur_exp * log1p(-pval));
        }
        if (pv_sidak_sd < pv_sidak_sd_new) {
          pv_sidak_sd = pv_sidak_sd_new;
        }
        adjust_print(output_min_p_buf, pv_sidak_sd, output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (fdrbh_col) {
        adjust_print(output_min_p_buf, pv_bh[vidx], output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      if (pv_by) {
        adjust_print(output_min_p_buf, pv_by[vidx], output_min_p, output_min_p_slen, is_log10, &cswritep);
      }
      append_binary_eoln(&cswritep);
      if (cswrite(&css, &cswritep)) {
        goto multcomp_ret_WRITE_FAIL;
      }
    }
    if (cswrite_close_null(&css, cswritep)) {
      goto multcomp_ret_WRITE_FAIL;
    }
    // don't use valid_variant_ct due to --pfilter
    LOGPRINTFWW("--adjust%s values (%u variant%s) written to %s .\n", cip? "" : "-file", vidx, (vidx == 1)? "" : "s", outname);
  }
  while (0) {
  multcomp_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  multcomp_ret_WRITE_FAIL:
    reterr = kPglRetWriteFail;
    break;
  }
 multcomp_ret_1:
  cswrite_close_cond(&css, cswritep);
  bigstack_reset(bigstack_mark);
  return reterr;
}

pglerr_t adjust_file(__maybe_unused const adjust_file_info_t* afip, __maybe_unused double pfilter, __maybe_unused double output_min_p, __maybe_unused uint32_t max_thread_ct, __maybe_unused char* outname, __maybe_unused char* outname_end) {
  logerrprint("Error: --adjust-file is currently under development.\n");
  return kPglRetNotYetSupported;
    /*
  unsigned char* bigstack_mark = g_bigstack_base;
  unsigned char* bigstack_end_mark = g_bigstack_end;
  gzFile gz_infile = nullptr;
  uintptr_t loadbuf_size = 0;
  uintptr_t line_idx = 0;
  pglerr_t reterr = kPglRetSuccess;
  {
    // Two-pass load.
    // 1. Parse header line, count # of variants.
    // intermission. Allocate top-level arrays.
    // 2. Rewind and fill arrays.
    // (some overlap with load_pvar(), though that's one-pass.)
    reterr = gzopen_read_checked(afip->fname, &gz_infile);
    if (reterr) {
      goto adjust_file_ret_1;
    }
    loadbuf_size = bigstack_left() / 4;
    if (loadbuf_size > kMaxLongLine) {
      loadbuf_size = kMaxLongLine;
    } else if (loadbuf_size >= kMaxMediumLine + kCacheline) {
      loadbuf_size = round_down_pow2(loadbuf_size, kCacheline);
    } else {
      goto adjust_file_ret_NOMEM;
    }
    char* loadbuf = (char*)bigstack_end_alloc_raw(loadbuf_size);
    loadbuf[loadbuf_size - 1] = ' ';

    char* loadbuf_first_token;
    do {
      ++line_idx;
      if (!gzgets(gz_infile, loadbuf, loadbuf_size)) {
        if (!gzeof(gz_infile)) {
          goto adjust_file_ret_READ_FAIL;
        }
        snprintf(g_logbuf, kLogbufSize, "Error: %s is empty.\n", afip->fname);
        goto adjust_file_ret_MALFORMED_INPUT_WW;
      }
      if (!loadbuf[loadbuf_size - 1]) {
        goto adjust_file_ret_LONG_LINE;
      }
      loadbuf_first_token = skip_initial_spaces(loadbuf);
    } while (strequal_k2(loadbuf_first_token, "##"));
    if (*loadbuf_first_token == '#') {
      ++loadbuf_first_token;
    }

    const adjust_flags_t flags = afip->base.flags;
    // [0] = CHROM
    // [1] = POS
    // [2] = ID (required)
    // [3] = REF
    // [4] = ALT
    // [5] = TEST (always scan)
    // [6] = P (required)
    const char* col_search_order[7];
    col_search_order[0] = (flags & kfAdjustColChrom)? (afip->chr_field? afip->chr_field : "CHROM\0CHR\0") : "";
    col_search_order[1] = (flags & kfAdjustColPos)? (afip->pos_field? afip->pos_field : "POS\0BP\0") : "";
    col_search_order[2] = afip->id_field? afip->id_field : "ID\0SNP\0";
    col_search_order[3] = (flags & kfAdjustColRef)? (afip->ref_field? afip->ref_field : "REF\0A2\0") : "";
    col_search_order[4] = (flags & (kfAdjustColAlt1 | kfAdjustColAlt))? (afip->alt_field? afip->alt_field : "ALT\0ALT1\0A1\0") : "";
    col_search_order[5] = afip->test_field? afip->test_field : "TEST\0";
    col_search_order[6] = afip->p_field? afip->p_field : "P\0";

    uint32_t col_skips[7];
    uint32_t col_types[7];
    uint32_t relevant_col_ct;
    uint32_t found_type_bitset;
    reterr = get_header_line_col_nums(loadbuf_first_token, col_search_order, "adjust-file", 7, &relevant_col_ct, &found_type_bitset, col_skips, col_types);
    if (reterr) {
      goto adjust_file_ret_1;
    }
    if ((found_type_bitset & 0x44) != 0x44) {
      logerrprint("Error: --adjust-file requires ID and P columns.\n");
      goto adjust_file_ret_INCONSISTENT_INPUT;
    }
    uint32_t variant_ct = 0;
    uint32_t max_allele_slen = 1;

    if (gzrewind(gz_infile)) {
      goto adjust_file_ret_READ_FAIL;
    }
    line_idx = 0;
    const uint32_t variant_ctl = BITCT_TO_WORDCT(variant_ct);
    uintptr_t* variant_include_dummy;
    if (bigstack_alloc_ul(variant_ctl, &variant_include_dummy)) {
      goto adjust_file_ret_NOMEM;
    }
    fill_all_bits(variant_ct, variant_include_dummy);
    char** chr_ids = nullptr;
    uint32_t* variant_bps = nullptr;
    char** variant_ids = nullptr;
    uintptr_t* variant_allele_idxs = nullptr;
    char** allele_storage = nullptr;
    double* pvals = nullptr;
    do {
      ++line_idx;
      if (!gzgets(gz_infile, loadbuf, loadbuf_size)) {
        goto adjust_file_ret_READ_FAIL;
      }
      if (!loadbuf[loadbuf_size - 1]) {
        goto adjust_file_ret_READ_FAIL;
      }
      loadbuf_first_token = skip_initial_spaces(loadbuf);
    } while (strequal_k2(loadbuf_first_token, "##"));
    while (gzgets(gz_infile, loadbuf, loadbuf_size)) {
      ++line_idx;
      if (!loadbuf[loadbuf_size - 1]) {
        goto adjust_file_ret_READ_FAIL;
      }
      const char* loadbuf_iter = skip_initial_spaces(loadbuf);
      if (is_eoln_kns(*loadbuf_iter)) {
        continue;
      }
    }
    if ((!gzeof(gz_infile)) || gzclose_null(&gz_infile)) {
      goto adjust_file_ret_READ_FAIL;
    }
    bigstack_end_reset(bigstack_end_mark);
    reterr = multcomp(variant_include_dummy, nullptr, TO_CONSTCPCONSTP(chr_ids), variant_bps, TO_CONSTCPCONSTP(variant_ids), variant_allele_idxs, TO_CONSTCPCONSTP(allele_storage), &(afip->base), pvals, nullptr, variant_ct, max_allele_slen, pfilter, output_min_p, 0, max_thread_ct, outname, outname_end);
    if (reterr) {
      goto adjust_file_ret_1;
    }
  }
 adjust_file_ret_1:
  while (0) {
  adjust_file_ret_LONG_LINE:
    if (loadbuf_size == kMaxLongLine) {
      LOGERRPRINTFWW("Error: Line %" PRIuPTR " of %s is pathologically long.\n", line_idx, afip->fname);
      reterr = kPglRetMalformedInput;
      break;
    }
  adjust_file_ret_NOMEM:
    reterr = kPglRetNomem;
    break;
  adjust_file_ret_READ_FAIL:
    reterr = kPglRetReadFail;
    break;
  adjust_file_ret_MALFORMED_INPUT_WW:
    wordwrapb(0);
    logerrprintb();
    reterr = kPglRetMalformedInput;
    break;
  adjust_file_ret_INCONSISTENT_INPUT:
    reterr = kPglRetInconsistentInput;
    break;
  }
  gzclose_cond(gz_infile);
  bigstack_double_reset(bigstack_mark, bigstack_end_mark);
  return reterr;
    */
}

#ifdef __cplusplus
} // namespace plink2
#endif
