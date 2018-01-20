// Copyright (c) 2018, The Gullveig Project. Partial Copyright (c) 2014-2017 The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/shared_ptr.hpp>

#include "common/varint.h"
#include "warnings.h"
#include "crypto.h"
#include "hash.h"

namespace crypto {

  using std::abort;
  using std::int32_t;
  using std::int64_t;
  using std::size_t;
  using std::uint32_t;
  using std::uint64_t;

  extern "C" {
#include "crypto-ops.h"
#include "random.h"
  }

  boost::mutex random_lock;

  static inline unsigned char *operator &(ec_point &point) {
    return &reinterpret_cast<unsigned char &>(point);
  }

  static inline const unsigned char *operator &(const ec_point &point) {
    return &reinterpret_cast<const unsigned char &>(point);
  }

  static inline unsigned char *operator &(ec_scalar &scalar) {
    return &reinterpret_cast<unsigned char &>(scalar);
  }

  static inline const unsigned char *operator &(const ec_scalar &scalar) {
    return &reinterpret_cast<const unsigned char &>(scalar);
  }

  /* generate a random 32-byte (256-bit) integer and copy it to res */
  static inline void random_scalar_not_thread_safe(ec_scalar &res) {
    unsigned char tmp[64];
    generate_random_bytes_not_thread_safe(64, tmp);
    sc_reduce(tmp);
    memcpy(&res, tmp, 32);
  }
  static inline void random_scalar(ec_scalar &res) {
    boost::lock_guard<boost::mutex> lock(random_lock);
    random_scalar_not_thread_safe(res);
  }

  static inline void hash_to_scalar(const void *data, size_t length, ec_scalar &res) {
    cn_fast_hash(data, length, reinterpret_cast<hash &>(res));
    sc_reduce32(&res);
  }

  /* 
   * generate public and secret keys from a random 256-bit integer
   * TODO: allow specifiying random value (for wallet recovery)
   * 
   */
  secret_key crypto_ops::generate_keys(public_key &pub, secret_key &sec, const secret_key& recovery_key, bool recover) {
    ge_p3 point;

    secret_key rng;

    if (recover)
    {
      rng = recovery_key;
    }
    else
    {
      random_scalar(rng);
    }
    sec = rng;
    sc_reduce32(&sec);  // reduce in case second round of keys (sendkeys)

    ge_scalarmult_base(&point, &sec);
    ge_p3_tobytes(&pub, &point);

    return rng;
  }

  bool crypto_ops::check_key(const public_key &key) {
    ge_p3 point;
    return ge_frombytes_vartime(&point, &key) == 0;
  }

  bool crypto_ops::secret_key_to_public_key(const secret_key &sec, public_key &pub) {
    ge_p3 point;
    if (sc_check(&sec) != 0) {
      return false;
    }
    ge_scalarmult_base(&point, &sec);
    ge_p3_tobytes(&pub, &point);
    return true;
  }

  bool crypto_ops::generate_key_derivation(const public_key &key1, const secret_key &key2, key_derivation &derivation) {
    ge_p3 point;
    ge_p2 point2;
    ge_p1p1 point3;
    assert(sc_check(&key2) == 0);
    if (ge_frombytes_vartime(&point, &key1) != 0) {
      return false;
    }
    ge_scalarmult(&point2, &key2, &point);
    ge_mul8(&point3, &point2);
    ge_p1p1_to_p2(&point2, &point3);
    ge_tobytes(&derivation, &point2);
    return true;
  }

  void crypto_ops::derivation_to_scalar(const key_derivation &derivation, size_t output_index, ec_scalar &res) {
    struct {
      key_derivation derivation;
      char output_index[(sizeof(size_t) * 8 + 6) / 7];
    } buf;
    char *end = buf.output_index;
    buf.derivation = derivation;
    tools::write_varint(end, output_index);
    assert(end <= buf.output_index + sizeof buf.output_index);
    hash_to_scalar(&buf, end - reinterpret_cast<char *>(&buf), res);
  }

  bool crypto_ops::derive_public_key(const key_derivation &derivation, size_t output_index,
    const public_key &base, public_key &derived_key) {
    ec_scalar scalar;
    ge_p3 point1;
    ge_p3 point2;
    ge_cached point3;
    ge_p1p1 point4;
    ge_p2 point5;
    if (ge_frombytes_vartime(&point1, &base) != 0) {
      return false;
    }
    derivation_to_scalar(derivation, output_index, scalar);
    ge_scalarmult_base(&point2, &scalar);
    ge_p3_to_cached(&point3, &point2);
    ge_add(&point4, &point1, &point3);
    ge_p1p1_to_p2(&point5, &point4);
    ge_tobytes(&derived_key, &point5);
    return true;
  }

  void crypto_ops::derive_secret_key(const key_derivation &derivation, size_t output_index,
    const secret_key &base, secret_key &derived_key) {
    ec_scalar scalar;
    assert(sc_check(&base) == 0);
    derivation_to_scalar(derivation, output_index, scalar);
    sc_add(&derived_key, &base, &scalar);
  }

  struct s_comm {
    hash h;
    ec_point key;
    ec_point comm;
  };

  struct s_comm_2 {
    hash msg;
    ec_point D;
    ec_point X;
    ec_point Y;
  };

  void crypto_ops::generate_signature(const hash &prefix_hash, const public_key &pub, const secret_key &sec, signature &sig) {
    ge_p3 tmp3;
    ec_scalar k;
    s_comm buf;
#if !defined(NDEBUG)
    {
      ge_p3 t;
      public_key t2;
      assert(sc_check(&sec) == 0);
      ge_scalarmult_base(&t, &sec);
      ge_p3_tobytes(&t2, &t);
      assert(pub == t2);
    }
#endif
    buf.h = prefix_hash;
    buf.key = pub;
    random_scalar(k);
    ge_scalarmult_base(&tmp3, &k);
    ge_p3_tobytes(&buf.comm, &tmp3);
    hash_to_scalar(&buf, sizeof(s_comm), sig.c);
    sc_mulsub(&sig.r, &sig.c, &sec, &k);
  }

  bool crypto_ops::check_signature(const hash &prefix_hash, const public_key &pub, const signature &sig) {
    ge_p2 tmp2;
    ge_p3 tmp3;
    ec_scalar c;
    s_comm buf;
    assert(check_key(pub));
    buf.h = prefix_hash;
    buf.key = pub;
    if (ge_frombytes_vartime(&tmp3, &pub) != 0) {
      return false;
    }
    if (sc_check(&sig.c) != 0 || sc_check(&sig.r) != 0) {
      return false;
    }
    ge_double_scalarmult_base_vartime(&tmp2, &sig.c, &tmp3, &sig.r);
    ge_tobytes(&buf.comm, &tmp2);
    hash_to_scalar(&buf, sizeof(s_comm), c);
    sc_sub(&c, &c, &sig.c);
    return sc_isnonzero(&c) == 0;
  }

  void crypto_ops::generate_tx_proof(const hash &prefix_hash, const public_key &R, const public_key &A, const public_key &D, const secret_key &r, signature &sig) {
    // sanity check
    ge_p3 R_p3;
    ge_p3 A_p3;
    ge_p3 D_p3;
    if (ge_frombytes_vartime(&R_p3, &R) != 0) throw std::runtime_error("tx pubkey is invalid");
    if (ge_frombytes_vartime(&A_p3, &A) != 0) throw std::runtime_error("recipient view pubkey is invalid");
    if (ge_frombytes_vartime(&D_p3, &D) != 0) throw std::runtime_error("key derivation is invalid");
#if !defined(NDEBUG)
    {
      assert(sc_check(&r) == 0);
      // check R == r*G
      ge_p3 dbg_R_p3;
      ge_scalarmult_base(&dbg_R_p3, &r);
      public_key dbg_R;
      ge_p3_tobytes(&dbg_R, &dbg_R_p3);
      assert(R == dbg_R);
      // check D == r*A
      ge_p2 dbg_D_p2;
      ge_scalarmult(&dbg_D_p2, &r, &A_p3);
      public_key dbg_D;
      ge_tobytes(&dbg_D, &dbg_D_p2);
      assert(D == dbg_D);
    }
#endif

    // pick random k
    ec_scalar k;
    random_scalar(k);
    
    // compute X = k*G
    ge_p3 X_p3;
    ge_scalarmult_base(&X_p3, &k);
    
    // compute Y = k*A
    ge_p2 Y_p2;
    ge_scalarmult(&Y_p2, &k, &A_p3);

    // sig.c = Hs(Msg || D || X || Y)
    s_comm_2 buf;
    buf.msg = prefix_hash;
    buf.D = D;
    ge_p3_tobytes(&buf.X, &X_p3);
    ge_tobytes(&buf.Y, &Y_p2);
    hash_to_scalar(&buf, sizeof(s_comm_2), sig.c);

    // sig.r = k - sig.c*r
    sc_mulsub(&sig.r, &sig.c, &r, &k);
  }

  bool crypto_ops::check_tx_proof(const hash &prefix_hash, const public_key &R, const public_key &A, const public_key &D, const signature &sig) {
    // sanity check
    ge_p3 R_p3;
    ge_p3 A_p3;
    ge_p3 D_p3;
    if (ge_frombytes_vartime(&R_p3, &R) != 0) return false;
    if (ge_frombytes_vartime(&A_p3, &A) != 0) return false;
    if (ge_frombytes_vartime(&D_p3, &D) != 0) return false;
    if (sc_check(&sig.c) != 0 || sc_check(&sig.r) != 0) return false;

    // compute sig.c*R
    ge_p2 cR_p2;
    ge_scalarmult(&cR_p2, &sig.c, &R_p3);

    // compute sig.r*G
    ge_p3 rG_p3;
    ge_scalarmult_base(&rG_p3, &sig.r);

    // compute sig.c*D
    ge_p2 cD_p2;
    ge_scalarmult(&cD_p2, &sig.c, &D_p3);

    // compute sig.r*A
    ge_p2 rA_p2;
    ge_scalarmult(&rA_p2, &sig.r, &A_p3);

    // compute X = sig.c*R + sig.r*G
    public_key cR;
    ge_tobytes(&cR, &cR_p2);
    ge_p3 cR_p3;
    if (ge_frombytes_vartime(&cR_p3, &cR) != 0) return false;
    ge_cached rG_cached;
    ge_p3_to_cached(&rG_cached, &rG_p3);
    ge_p1p1 X_p1p1;
    ge_add(&X_p1p1, &cR_p3, &rG_cached);
    ge_p2 X_p2;
    ge_p1p1_to_p2(&X_p2, &X_p1p1);

    // compute Y = sig.c*D + sig.r*A
    public_key cD;
    public_key rA;
    ge_tobytes(&cD, &cD_p2);
    ge_tobytes(&rA, &rA_p2);
    ge_p3 cD_p3;
    ge_p3 rA_p3;
    if (ge_frombytes_vartime(&cD_p3, &cD) != 0) return false;
    if (ge_frombytes_vartime(&rA_p3, &rA) != 0) return false;
    ge_cached rA_cached;
    ge_p3_to_cached(&rA_cached, &rA_p3);
    ge_p1p1 Y_p1p1;
    ge_add(&Y_p1p1, &cD_p3, &rA_cached);
    ge_p2 Y_p2;
    ge_p1p1_to_p2(&Y_p2, &Y_p1p1);

    // compute c2 = Hs(Msg || D || X || Y)
    s_comm_2 buf;
    buf.msg = prefix_hash;
    buf.D = D;
    ge_tobytes(&buf.X, &X_p2);
    ge_tobytes(&buf.Y, &Y_p2);
    ec_scalar c2;
    hash_to_scalar(&buf, sizeof(s_comm_2), c2);

    // test if c2 == sig.c
    sc_sub(&c2, &c2, &sig.c);
    return sc_isnonzero(&c2) == 0;
  }

  static void hash_to_ec(const public_key &key, ge_p3 &res) {
    hash h;
    ge_p2 point;
    ge_p1p1 point2;
    cn_fast_hash(std::addressof(key), sizeof(public_key), h);
    ge_fromfe_frombytes_vartime(&point, reinterpret_cast<const unsigned char *>(&h));
    ge_mul8(&point2, &point);
    ge_p1p1_to_p3(&res, &point2);
  }

  void crypto_ops::generate_key_image(const public_key &pub, const secret_key &sec, key_image &image) {
    ge_p3 point;
    ge_p2 point2;
    assert(sc_check(&sec) == 0);
    hash_to_ec(pub, point);
    ge_scalarmult(&point2, &sec, &point);
    ge_tobytes(&image, &point2);
  }

PUSH_WARNINGS
DISABLE_VS_WARNINGS(4200)
  struct ec_point_pair {
    ec_point a, b;
  };
  struct rs_comm {
    hash h;
    struct ec_point_pair ab[];
  };
POP_WARNINGS

  static inline size_t rs_comm_size(size_t pubs_count) {
    return sizeof(rs_comm) + pubs_count * sizeof(ec_point_pair);
  }

  void crypto_ops::generate_ring_signature(const hash &prefix_hash, const key_image &image,
    const public_key *const *pubs, size_t pubs_count,
    const secret_key &sec, size_t sec_index,
    signature *sig) {
    size_t i;
    ge_p3 image_unp;
    ge_dsmp image_pre;
    ec_scalar sum, k, h;
    boost::shared_ptr<rs_comm> buf(reinterpret_cast<rs_comm *>(malloc(rs_comm_size(pubs_count))), free);
    if (!buf)
      abort();
    assert(sec_index < pubs_count);
#if !defined(NDEBUG)
    {
      ge_p3 t;
      public_key t2;
      key_image t3;
      assert(sc_check(&sec) == 0);
      ge_scalarmult_base(&t, &sec);
      ge_p3_tobytes(&t2, &t);
      assert(*pubs[sec_index] == t2);
      generate_key_image(*pubs[sec_index], sec, t3);
      assert(image == t3);
      for (i = 0; i < pubs_count; i++) {
        assert(check_key(*pubs[i]));
      }
    }
#endif
    if (ge_frombytes_vartime(&image_unp, &image) != 0) {
      abort();
    }
    ge_dsm_precomp(image_pre, &image_unp);
    sc_0(&sum);
    buf->h = prefix_hash;
    for (i = 0; i < pubs_count; i++) {
      ge_p2 tmp2;
      ge_p3 tmp3;
      if (i == sec_index) {
        random_scalar(k);
        ge_scalarmult_base(&tmp3, &k);
        ge_p3_tobytes(&buf->ab[i].a, &tmp3);
        hash_to_ec(*pubs[i], tmp3);
        ge_scalarmult(&tmp2, &k, &tmp3);
        ge_tobytes(&buf->ab[i].b, &tmp2);
      } else {
        random_scalar(sig[i].c);
        random_scalar(sig[i].r);
        if (ge_frombytes_vartime(&tmp3, &*pubs[i]) != 0) {
          abort();
        }
        ge_double_scalarmult_base_vartime(&tmp2, &sig[i].c, &tmp3, &sig[i].r);
        ge_tobytes(&buf->ab[i].a, &tmp2);
        hash_to_ec(*pubs[i], tmp3);
        ge_double_scalarmult_precomp_vartime(&tmp2, &sig[i].r, &tmp3, &sig[i].c, image_pre);
        ge_tobytes(&buf->ab[i].b, &tmp2);
        sc_add(&sum, &sum, &sig[i].c);
      }
    }
    hash_to_scalar(buf.get(), rs_comm_size(pubs_count), h);
    sc_sub(&sig[sec_index].c, &h, &sum);
    sc_mulsub(&sig[sec_index].r, &sig[sec_index].c, &sec, &k);
  }

  bool crypto_ops::check_ring_signature(const hash &prefix_hash, const key_image &image,
    const public_key *const *pubs, size_t pubs_count,
    const signature *sig) {
    size_t i;
    ge_p3 image_unp;
    ge_dsmp image_pre;
    ec_scalar sum, h;
    boost::shared_ptr<rs_comm> buf(reinterpret_cast<rs_comm *>(malloc(rs_comm_size(pubs_count))), free);
    if (!buf)
      return false;
#if !defined(NDEBUG)
    for (i = 0; i < pubs_count; i++) {
      assert(check_key(*pubs[i]));
    }
#endif
    if (ge_frombytes_vartime(&image_unp, &image) != 0) {
      return false;
    }
    ge_dsm_precomp(image_pre, &image_unp);
    sc_0(&sum);
    buf->h = prefix_hash;
    for (i = 0; i < pubs_count; i++) {
      ge_p2 tmp2;
      ge_p3 tmp3;
      if (sc_check(&sig[i].c) != 0 || sc_check(&sig[i].r) != 0) {
        return false;
      }
      if (ge_frombytes_vartime(&tmp3, &*pubs[i]) != 0) {
        return false;
      }
      ge_double_scalarmult_base_vartime(&tmp2, &sig[i].c, &tmp3, &sig[i].r);
      ge_tobytes(&buf->ab[i].a, &tmp2);
      hash_to_ec(*pubs[i], tmp3);
      ge_double_scalarmult_precomp_vartime(&tmp2, &sig[i].r, &tmp3, &sig[i].c, image_pre);
      ge_tobytes(&buf->ab[i].b, &tmp2);
      sc_add(&sum, &sum, &sig[i].c);
    }
    hash_to_scalar(buf.get(), rs_comm_size(pubs_count), h);
    sc_sub(&h, &h, &sum);
    return sc_isnonzero(&h) == 0;
  }
}
