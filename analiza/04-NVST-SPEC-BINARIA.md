# NVST — specyfikacja formatów binarnych dla implementacji w C

> **Cel:** dać implementatorowi w C dokładne formaty bajtowe, żeby nie musiał ich
> odtwarzać z 14 695 linii Rusta w `nvst.rs`.
>
> **Źródło:** `native/opennow-streamer/crates/opennow-streamer-transport/src/nvst.rs`
> (OpenNOW `main`, commit `a49b124`, licencja **MIT** — przepisywanie dozwolone z notą).
> Każda sekcja ma **numery linii**, żeby można było zweryfikować i doczytać kontekst.
>
> **Zakres:** tylko H.264 8-bit 4:2:0 po Mjolnirze. HEVC/AV1/HDR/mikrofon/gamepady
> są poza zakresem (patrz `HANDOFF.md` §5.2).
>
> **Konwencja:** `u32le(x)` = 4 bajty little-endian. Wszystkie offsety od zera.

---

## 1. Nagłówek RTP i rozszerzenie `0x4753` („GS")

### 1.1 Parsowanie stałego nagłówka RTP (12 B)

`nvst.rs:2440-2510`. Standardowo, ale zwróć uwagę na kolejność odczytu:

```c
uint8_t  first = p[0];
/* p[0] bit 7 = version (musi byc 2), bit 6 = padding (0x20), bit 5 = extension (0x10) */
uint8_t  payload_type = p[1] & 0x7f;
uint16_t seq          = be16(p + 2);
uint32_t timestamp    = be32(p + 4);
uint32_t ssrc         = be32(p + 8);
size_t   payload_off  = 12;   /* + CSRC, gdy CC > 0 */
```

Stała: `RTP_FIXED_HEADER_LEN = 12` (`nvst.rs:79`).

### 1.2 Rozszerzenie GS — **warunek konieczny dla wideo Mjolnir**

Jeśli `first & 0x10` (bit extension):

```c
uint16_t profile = be16(p + payload_off);        /* musi byc 0x4753 */
uint16_t words   = be16(p + payload_off + 2);    /* liczba slow 32-bitowych */
size_t   ext_len = (size_t)words * 4 + 4;        /* +4 za sam naglowek rozszerzenia */
if (profile != 0x4753)      -> to nie jest wideo NVST
if ((size_t)words * 4 != 16) -> Blad: InvalidExtensionLength   /* nvst.rs:2490 */
const uint8_t *gs = p + payload_off + 4;         /* 16 bajtow ciala */
payload_off += ext_len;                          /* = payload_off + 20 */
```

Stałe: `GS_VIDEO_EXTENSION_PROFILE = 0x4753` (`nvst.rs:269`),
`NV_VIDEO_PACKET_LEN = 16` (`nvst.rs:212`).

**Twardy wymóg:** `words * 4` musi być **dokładnie 16**. Test `gs_extension_requires_exactly_four_words`
(`nvst.rs:12099`) pilnuje tego. Brak rozszerzenia GS w uwierzytelnionym pakiecie wideo
→ odrzucamy reasemblację i żądamy PLI (`nvst.rs:12893`).

### 1.3 Ciało GS — 16 bajtów, **little-endian**

To jest najważniejsza tabela w całym dokumencie. Źródło: `NvVideoPacket::parse`,
`nvst.rs:3225-3250`.

```
offset  bajty  pole                      wydobycie
------  -----  ------------------------  -------------------------------------------
 0..4    4     packet_word  = u32le      stream_packet_index = (packet_word >> 8) & 0x00FF_FFFF
 4..8    4     frame_index  = u32le      (bezposrednio)
 8..12   4     flags_word   = u32le      flags               = flags_word & 0x0F
                                         fec_current_block   = (ext[11] >> 4) & 0x03
                                         fec_last_block      = (ext[11] >> 6) & 0x03
12..16   4     fec_word     = u32le      fec_percentage      = (fec_word >>  4) & 0xFF
                                         fec_index (shard)   = (fec_word >> 12) & 0x3FF
                                         fec_source_packets  = (fec_word >> 22) & 0x3FF
```

⚠️ **Pułapka:** `fec_current_block` i `fec_last_block` są czytane z **`ext[11]`**, czyli
z *czwartego* bajtu `flags_word` (najstarszy bajt słowa LE), a nie z osobnego pola.
`ext[11]` to `flags_word >> 24`.

⚠️ **Pułapka druga:** wszystko jest **little-endian**, w przeciwieństwie do nagłówka RTP,
który jest big-endian. Test `mjolnir-nack-v2.md` zwraca na to uwagę wprost przy kodach
komend („`0x0308` na drucie to `08 03`").

Stałe (`nvst.rs:263-266`):

```c
#define FLAG_CONTAINS_PIC_DATA 0x01
#define FLAG_EOF               0x02
#define FLAG_SOF               0x04
#define STREAM_PACKET_INDEX_MASK 0x00FFFFFFu   /* 24 bity */
```

Walidacja flag (`nvst.rs:4089`): `flags & ~(SOF|EOF|CONTAINS_PIC_DATA) != 0` → pakiet nieprawidłowy.

### 1.4 Granice klatki

`nvst.rs:3252-3257`:

```c
bool is_start_of_frame = (flags & FLAG_SOF) && (fec_current_block == 0);
bool is_end_of_frame   = (flags & FLAG_EOF) && (fec_current_block == fec_last_block);
bool is_fec_packet     = (fec_percentage != 0) && (fec_index >= fec_source_packets);
```

**Kluczowe:** pakiety FEC mają `fec_index >= fec_source_packets` — czyli najpierw idą
shardy danych (0..data_shards-1), potem naprawcze. To jest **kod systematyczny**.

Ciągłość `stream_packet_index` jest egzekwowana: oczekiwanie `last + 1 (mod 2^24)`
(`nvst.rs:4815`). Luka w indeksie GS → odrzucenie klatki i żądanie PLI
(`nvst.rs:12751`), a nowy SOF pozwala się z tego wydostać (`nvst.rs:12780`).

---

## 2. Od pakietu do access unitu H.264

### 2.1 Gdzie zaczyna się nagłówek ramki GameStream

Dokumentacja w `nvst.rs:3230-3234` mówi: *„The RTP payload starts with the GameStream
frame header on an SOF packet and otherwise continues the H.264 access-unit data directly."*

Ale **implementacja nie używa stałej długości**. Dla H.264/H.265 (`video_picture_payload`,
`nvst.rs:3481-3491`):

```c
size_t search_len = MIN(payload_len, MAX_GS_FRAME_HEADER_BYTES + 4);   /* = 68 */
size_t offset;
if (!find_annex_b_start_code(payload, search_len, &offset)) return NULL;
return payload + offset;
```

Czyli: **szukaj start code Annex-B (`00 00 01` lub `00 00 00 01`) w pierwszych 68 bajtach
payloadu i tnij tam.** To jest odporne na zmienną długość nagłówka GS.

Stałe (`nvst.rs:270-274`):
```c
#define MAX_GS_FRAME_HEADER_BYTES       64
#define GS_SHORT_FRAME_HEADER_BYTES      8    /* AV1: payload[0] == 0x01 */
#define GFN_EXTENDED_FRAME_HEADER_BYTES 20   /* AV1: payload[0] == 0x81 */
```

⚠️ **Pułapka:** wariant z **stałym offsetem 8 lub 20** jest używany **tylko dla AV1**
(`nvst.rs:3493-3494`), bo AV1 nie ma start code, po którym można by znaleźć granicę.
Komentarz w źródle ostrzega, że to *„similarly named but different consumer GameStream
extended-header layout"* — nie myl tych dwóch układów. **Dla H.264 zawsze szukaj start code.**

### 2.2 Kluczowa klatka

`video_access_unit_is_keyframe` (`nvst.rs:3500-3523`): skanuj NAL-e Annex-B, kluczowa gdy
`nal_header & 0x1f == 5` (IDR). Dla H.265: `(nal_header >> 1) & 0x3f` w zakresie 16..=21.

### 2.3 Odcinanie końcowego AUD

Funkcja `strip_trailing_access_unit_delimiter` (`nvst.rs:3458-3479`): jeśli **ostatni** NAL to AUD
(`nal_header & 0x1f == 9` dla H.264), **odetnij go** — należy do następnej klatki:

```c
/* H.264: kind 9 = Access Unit Delimiter */
if (last_nal && (bytes[nal_start] & 0x1f) == 9) truncate(last_nal_start);
```

To jest detal, którego pominięcie daje dekoderowi śmieci na końcu każdej klatki.

### 2.4 Parametry zbierania access unitu

| Parametr | Wartość | Linia |
|---|---|---|
| `DEFAULT_MAX_ACCESS_UNIT_BYTES` | 2 MiB | `nvst.rs:236` |
| `MAX_ACCESS_UNIT_BYTES` | 16 MiB | `nvst.rs:237` |
| `MAX_GS_FRAME_HEADER_BYTES` | 64 | `nvst.rs:270` |

Kolejka zakodowanego wideo w OpenNOW ma **pojemność 2** (`docs/streamer-comparison/video.md`
pkt 5); przepełnienie oznacza desync i żądanie klatki kluczowej. Na tej maszynie (2 GB RAM)
trzymaj pojemność 2-3, nie więcej.

---

## 3. SRTP — profile, KDF, IV, weryfikacja

### 3.1 Obsługiwane profile i ich nazwy z SDP

`nvst.rs:301-329`. Nazwy akceptowane z `a=crypto:` / `advertised_srtp_profile`:

| Ciąg w SDP | Profil | Klucz | Salt sesji | Tag |
|---|---|---|---|---|
| `AES_CM_128_HMAC_SHA1_80` | `AesCm128HmacSha1_80` | 16 B | 14 B | 10 B |
| `AES_CM_128_HMAC_SHA1_32` | `AesCm128HmacSha1_32` | 16 B | 14 B | 4 B |
| `AES_CM_256_HMAC_SHA1_80` | `AesCm256HmacSha1_80` | 32 B | 14 B | 10 B |
| `AES_CM_256_HMAC_SHA1_32` | `AesCm256HmacSha1_32` | 32 B | 14 B | 4 B |
| `AEAD_AES_128_GCM` / `SRTP_AEAD_AES_128_GCM` | `AeadAes128Gcm` | 16 B | 12 B | 16 B |
| `AEAD_AES_256_GCM` / `SRTP_AEAD_AES_256_GCM` | `AeadAes256Gcm` | 32 B | 12 B | 16 B |
| `AEAD_AES_128_GCM_8` / `SRTP_AEAD_AES_128_GCM_8` | `AeadAes128Gcm8` | 16 B | 12 B | **8 B** |
| `AEAD_AES_256_GCM_8` / `SRTP_AEAD_AES_256_GCM_8` | `AeadAes256Gcm8` | 32 B | 12 B | **8 B** |

Stałe długości tagów: `SRTP_AES_CM_HMAC_SHA1_80_TAG_LEN = 10` (`nvst.rs:81`),
`SRTP_AEAD_AES_GCM_TAG_LEN = 16` (`nvst.rs:83`), `SRTP_AEAD_AES_GCM_8_TAG_LEN = 8` (`nvst.rs:86`).

Komentarz przy `unprotect_aes_gcm` (`nvst.rs:2708-2790`) jest istotny: *„`aes-gcm` 0.10 only
seals 12–16 byte tags; NVIDIA's Mjolnir path uses the 8-byte `aes_gcm_*_8_auth` libsrtp
policy."* Czyli **wariant GCM_8 jest realnie używany** przez chmurę — nie pomijaj go.
W C z OpenSSL/mbedTLS oblicz pełny 16-bajtowy tag i **porównaj tylko pierwsze 8 bajtów**
(to jest dokładnie to, co robi OpenNOW: maskowanie tagu przez `Ctr32BE` od `j0`,
`nvst.rs:2774-2783`).

### 3.2 Skąd biorą się klucze master

`nvst.rs:1586-1640`. Z odpowiedzi RTSP (JSON) — pola **hex jako stringi**:

```
srtpAesKeyHex  -> master_key   (16 B dla CM_128/GCM_128, 32 B dla CM_256/GCM_256)
srtpSaltHex    -> master_salt  (14 B dla AES-CM, 12 B dla GCM)
```

Walidacja: `decode_fixed_hex::<16>` / `::<32>` — **dokładna** długość, inaczej
`NvstConfigError::InvalidAesKey`. Nigdy nie loguj tych wartości: `Debug` dla
`NvstSrtpMaterial` drukuje `"[redacted]"` (`nvst.rs:1496-1497`). **Powtórz to.**

### 3.3 KDF — RFC 3711 AES-CM, dosłownie

`derive_aes_cm_key` (`nvst.rs:2873-2887`) i `derive_aes128_cm_key` (`nvst.rs:2888-2901`):

```c
/* wywodzenie N bajtow z master_key/master_salt i etykiety */
void srtp_kdf(const uint8_t *master_key, size_t key_len,   /* 16 lub 32 */
              const uint8_t *master_salt, size_t salt_len, /* 14 lub 12 */
              uint8_t label, uint8_t *out, size_t out_len)
{
    uint8_t iv[16] = {0};
    memcpy(iv, master_salt, salt_len);
    iv[7] ^= label;                       /* key_id = label * 2^48; po >>6 trafia do iv[7] */
    aes_ctr_keystream(master_key, key_len, iv, out, out_len);   /* out = ENC(00..00) */
}
```

**Etykiety** (`nvst.rs:90-94` + `2620-2626`):

| Materiał | Etykieta | Długość |
|---|---|---|
| `encryption_key` (SRTP) | **0x00** | 16 lub 32 |
| `authentication_key` (HMAC-SHA1) | **0x01** | **20** |
| `session_salt` (SRTP) | **0x02** | 14 (CM) / 12 (GCM) |
| `encryption_key` (SRTCP) | **0x03** | 16 lub 32 |
| `session_salt` (SRTCP) | **0x05** | 14 / 12 |

Komentarz w źródle: *„RFC 3711 SRTCP KDF labels (same master key, separate session keys)"*.
Czyli SRTCP używa **tego samego master_key/salt**, ale innych etykiet.

Tryb licznika: `Ctr128BE<Aes128|Aes256>` dla KDF (`nvst.rs:292-293`),
`Ctr32BE<Aes*>` dla strumienia danych (`nvst.rs:2745-2746`). **To nie jest to samo** —
`Ctr128BE` inkrementuje całe 128-bitowe słowo, `Ctr32BE` tylko ostatnie 32 bity.
RFC 3711 wymaga `Ctr32BE` dla danych i pełnego `k_i` dla KDF. Pomieszanie ich daje
cichą korupcję, nie błąd.

### 3.4 IV dla AES-CM (16 B)

`srtp_aes_cm_iv` (`nvst.rs:2902-2914`):

```c
uint8_t iv[16] = {0};
memcpy(iv, session_salt, 14);          /* session_salt ma 14 bajtow */
/* iv[4..8] ^= be32(ssrc) */
for (int i = 0; i < 4; i++) iv[4 + i] ^= (uint8_t)(ssrc >> (24 - 8*i));
/* iv[8..14] ^= dolne 6 bajtow be64(packet_index) */
uint64_t pi_be = htobe64(packet_index);
for (int i = 0; i < 6; i++) iv[8 + i] ^= ((uint8_t*)&pi_be)[2 + i];
```

To jest dokładnie `IV = (session_salt × 2^16) XOR (packet_index × 2^64)` z RFC 3711 —
sprawdź sobie przesunięcia, bo `iv[4..8]` dla SSRC wygląda nieintuicyjnie.

### 3.5 IV dla AES-GCM (12 B)

`srtp_gcm_iv` (`nvst.rs:2915-2928`):

```c
uint8_t iv[12];
memcpy(iv, session_salt, 12);
for (int i = 0; i < 4; i++) iv[2 + i] ^= (uint8_t)(ssrc >> (24 - 8*i));   /* iv[2..6]  */
for (int i = 0; i < 4; i++) iv[6 + i] ^= (uint8_t)(roc  >> (24 - 8*i));   /* iv[6..10] */
for (int i = 0; i < 2; i++) iv[10 + i] ^= (uint8_t)(seq >> (8 - 8*i));    /* iv[10..12] */
```

### 3.6 Indeks pakietu, ROC i kolejność operacji

`SrtpReceiver::unprotect` (`nvst.rs:2635-2705`). **Kolejność jest istotna:**

```c
1  header        = rtp_parse(datagram);
2  packet_index  = replay_guess_index(header.seq);      /* 48-bitowy: (ROC << 16) | seq */
3  roc           = (uint32_t)(packet_index >> 16);
4  plaintext     = unprotect_(gcm|cm_hmac)(datagram, header, roc/packet_index, ...);
5  replay_check(packet_index);                          /* DOPIERO po weryfikacji tagu */
6  header.payload(&plaintext);                          /* walidacja dlugosci */
7  replay_commit(packet_index);                         /* commit na samym koncu */
```

Dlaczego tak: `check` przed `commit` i **po** uwierzytelnieniu oznacza, że pakiet
z błędnym tagiem nie przesuwa okna replay. Zmiana tej kolejności otwiera prosty DoS.

Okno replay: `SRTP_REPLAY_WINDOW_PACKETS = MAX_REORDER_WINDOW = 2048`,
`SRTP_REPLAY_WINDOW_WORDS = 2048/64 = 32` słowa (`nvst.rs:233-234`).
Okno reorder: `DEFAULT_REORDER_WINDOW = 1024`, `MAX_REORDER_WINDOW = 2048` (`nvst.rs:224-225`).

### 3.7 HMAC-SHA1 dla AES-CM

`unprotect_aes_cm_hmac_sha1` (`nvst.rs:2820-2872`):

```
mac = HMAC-SHA1(authentication_key /*20B*/,
                pakiet_RTP_od_bajtu_0_do_payload_offset
                ++ ciphertext
                ++ be32(roc))
tag = mac[0 .. authentication_tag_len]              /* 10 lub 4 bajty */
```

⚠️ `roc` jest doklejane jako **4 bajty big-endian** (`mac.update(&roc.to_be_bytes())`,
`nvst.rs:2845`). Pominięcie tego daje poprawnie wyglądające, ale zawsze odrzucane pakiety.

Porównanie tagu musi być **stałoczasowe** — OpenNOW używa `subtle::ConstantTimeEq`
(`nvst.rs:27`). W C: `CRYPTO_memcmp()` (OpenSSL) albo własna redukcja przez OR.

### 3.8 SRTCP

`nvst.rs:4357-4372` + stałe `95-98`:

```c
#define SRTCP_ENCRYPTED_FLAG 0x80000000u
#define RTCP_SENDER_SSRC     0x4F4E4F57u   /* "ONOW" - nasza wlasna tozsamosc */
/* SRTCP_RR_INTERVAL = 1 s */
```

SRTCP używa etykiet **0x03** (key) i **0x05** (salt) z tego samego mastera.

---

## 4. FEC — systematyczny Reed-Solomon Cauchy'ego nad GF(2^8)

### 4.1 Parametry bloku z pakietu

`struct FecPacketLayout` (`nvst.rs:3668`) i `FecPacketLayout::from_packet` (`nvst.rs:3677-3701`):

```c
uint32_t frame_index    = u32le(ext + 4);
uint32_t fec_word       = u32le(ext + 12);
unsigned repair_percent = (fec_word >>  4) & 0xFF;
unsigned shard_index    = (fec_word >> 12) & 0x3FF;
unsigned data_shards    = (fec_word >> 22) & 0x3FF;

if (data_shards == 0 || repair_percent == 0) -> FEC nieaktywne dla tego pakietu
parity_shards = ceil(data_shards * repair_percent / 100);
total_shards  = data_shards + parity_shards;
if (parity_shards == 0)            -> odrzuc
if (shard_index >= total_shards)   -> odrzuc
if (total_shards > 1023)           -> odrzuc        /* nvst.rs:3691 */

block_index      = (ext[11] >> 4) & 0x03;
last_block_index = (ext[11] >> 6) & 0x03;
```

⚠️ **`parity_shards` jest WYLICZANE, nie przesyłane.** Używa `ceil` (w Ruście
`saturating_mul(repair_percent).div_ceil(100)`). Dla `repair_percent = 20` i
`data_shards = 10` → `parity_shards = 2`. Dla `data_shards = 7` → `ceil(1.4) = 2`.
Błąd zaokrąglenia = całkowita utrata zdolności naprawy.

`MAX_PENDING_FEC_BLOCKS = 128` (`nvst.rs:211`).
`NVST_FEC_RTP_HEADER_ALLOWANCE = 16` (`nvst.rs:215`) — zapas na nagłówek RTP przy
szacowaniu rozmiaru pakietu FEC.

### 4.2 Arytmetyka GF(2^8)

`gf256_tables` (`nvst.rs:3717-3742`). **Wielomian pierwotny `0x11d`** — komentarz w źródle:
*„NVIDIA's GameStream FEC uses GF(2^8) with primitive polynomial 0x11d."*

```c
uint8_t exp_tab[512], log_tab[256];
uint16_t v = 1;
for (int power = 0; power < 255; power++) {
    exp_tab[power] = (uint8_t)v;
    log_tab[v]     = (uint8_t)power;
    v <<= 1;
    if (v & 0x100) v ^= 0x11d;
}
for (int power = 255; power < 512; power++) exp_tab[power] = exp_tab[power - 255];

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    return exp_tab[(unsigned)log_tab[a] + (unsigned)log_tab[b]];   /* bez modulo - stad 512 */
}
static inline uint8_t gf_inv(uint8_t a) {          /* a != 0 */
    return exp_tab[255 - (unsigned)log_tab[a]];
}
```

`gf256_multiply` (`nvst.rs:3744`), `gf256_inverse` (`nvst.rs:3753`),
`gf256_axpy` (`nvst.rs:3760-3775`), `gf256_scale` (`nvst.rs:3776-3783`):
```c
void gf_axpy(uint8_t *dst, const uint8_t *src, uint8_t c, size_t n); /* dst[i] ^= c*src[i] */
void gf_scale(uint8_t *buf, uint8_t c, size_t n);                    /* buf[i] *= c      */
```
Obie mają szybkie ścieżki dla `c == 0` (nic) i `c == 1` (czysty XOR) — **zachowaj je**,
bo to jest większość wywołań i na Meromie robi różnicę.

### 4.3 Macierz Cauchy'ego — współczynnik

`nvst_cauchy_coefficient` (`nvst.rs:3785-3793`):

```c
/* data_index: 0..data_shards-1 ; parity_index: 0..parity_shards-1 */
uint8_t xd = (uint8_t)(parity_shards + data_index);   /* moze przekroczyc u8 -> wtedy None */
uint8_t xp = (uint8_t)parity_index;
coefficient = gf_inv(xd ^ xp);                        /* 1 / (xd XOR xp) */
```

⚠️ Jeśli `parity_shards + data_index > 255`, współczynnik **nie istnieje**
(Rust zwraca `None` przez `u8::try_from`). Przy `total_shards <= 1023` to jest realnie
możliwe → trzeba to obsłużyć jako błąd, nie zawijać po cichu.

### 4.4 NAJWIĘKSZA PUŁAPKA W CAŁYM PROJEKCIE

Dosłowny cytat z `nvst.rs:3795-3797`:

> *„Reconstructs GameStream's systematic Cauchy Reed-Solomon data shards. Common generic
> Reed-Solomon crates use a different Vandermonde generator matrix; feeding NVIDIA parity
> to one produces **mathematically valid but corrupt H.264 packets**."*

Czyli: jeśli weźmiesz gotową bibliotekę RS (Jerasure, Leopard, `reed-solomon-erasure`,
`reedsolo`) i nakarmisz ją parzystością NVIDII, **dostaniesz pakiety, które przechodzą
weryfikację matematyczną, ale zawierają śmieci**. Objaw: dekoder dostaje uszkodzone
access unity, gubi referencje, żąda IDR w pętli — i nic nie wskazuje na FEC.

**Wniosek:** RS piszemy sami (~250-400 linii C z tablicami powyżej) albo bardzo dokładnie
weryfikujemy, że biblioteka używa **Cauchy'ego** z dokładnie tą samą konwencją indeksów.
Nie „prawie taką samą".

### 4.5 Rekonstrukcja i metadane transportowe

`reconstruct_nvst_cauchy_data` (`nvst.rs:3798+`). Klasyczna eliminacja Gaussa na
wierszach odpowiadających obecnym shardom.

⚠️ Druga pułapka — `nvst.rs:4103-4106`:

> *„it is transport metadata and can differ in a valid repaired data shard. The … a data
> shard caused every real cloud repair to be rejected even though the H.264 …"*

Czyli: **część bajtów w odtworzonym shardzie to metadane transportowe, które różnią się
między pakietem parzystości a oryginalnym pakietem danych.** Jeśli porównasz/zweryfikujesz
cały shard bajt po bajcie albo użyjesz tych pól bez poprawki, **każda realna naprawa
z chmury zostanie odrzucona** — mimo że dane H.264 są poprawne. Trzeba te pozycje
zignorować przy weryfikacji i/lub nadpisać poprawnymi wartościami z kontekstu
(`stream_packet_index`, `fec_index` itp. wynikają z pozycji w strumieniu, nie z shardu).

Przy naprawie licznik `fec_repaired` (`nvst.rs:3549`) rośnie o liczbę odzyskanych shardów.
Nieudana naprawa ustawia `repair_failed = true` i **nie jest ponawiana** dla tego bloku
(`nvst.rs:4228-4248`) — ale jest backup stanu przed próbą, więc blok nie zostaje zniszczony.

---

## 5. Reorder, NACK-v2 i odzyskiwanie

### 5.1 Stałe czasowe i pojemności (wszystkie z `nvst.rs:97-111`, `224-247`)

```c
#define SRTCP_RR_INTERVAL_MS         1000
#define RTCP_RECOVERY_INTERVAL_MS       4
#define NACK_RETRY_INTERVAL_MS            4
#define DEFAULT_NACK_RTT_MS              30   /* fallback, gdy brak swiezej probki RTT */
#define NACK_TRACKING_TIMEOUT_MS         52
#define MAX_NACK_ATTEMPTS                 3
#define RFI_BACKSTOP_TIMEOUT_MS         250
#define IDR_RETRY_INTERVAL_MS           100
#define MAX_PENDING_NACK_PACKETS       2048
#define MAX_PENDING_FRAME_ACKS          512
#define MAX_CONTROL_REPORT_BYTES       1071
#define MIN_CONTROL_REPORT_BYTES        (4 + FRAME_ACK_PAYLOAD_LEN)
#define STREAM_PING_TIMEOUT_MS         5000
#define MAX_PENDING_STREAM_PINGS         64
#define DEFAULT_REORDER_WINDOW         1024
#define MAX_REORDER_WINDOW             2048
#define MJOLNIR_REORDER_DEQUEUE_TIMEOUT_MS 52
#define NVST_UDP_RECEIVE_BUFFER_BYTES  (8*1024*1024)  /* -> na tej maszynie 1-2 MiB */
#define DEFAULT_TIMEOUT_MS             5000
#define MIN_TIMEOUT_MS                  250
#define MAX_TIMEOUT_MS                90000
#define MAX_PING_BYTES                  512
#define PING_INTERVAL_BEFORE_CONNECTION_MS 20
#define PING_INTERVAL_AFTER_CONNECTION_MS 100
#define UDP_RECEIVE_POLL_INTERVAL_MS     10
#define CONTROL_RECEIVE_POLL_INTERVAL_MS  1
#define MAX_NVST_VIDEO_PEER_PORTS        16
```

`DEFAULT_NVST_VIDEO_PACKET_SIZE = 1280`, `MIN = 256`, `MAX = 65519`,
`UDP_HEADER_BYTES = 8` (`nvst.rs:216-219`).

### 5.2 NACK-v2 — format bajtowy

Z `docs/mjolnir-nack-v2.md` (uzupełnione o `nvst_control.rs`):

Komenda ServerControl **`0x0317`** na kanale `control_channel_partially_reliable`.
Nagłówek ServerControl: `u16le(command)` + `u16le(payload_length)`.

```
offset  typ      znaczenie
------  -------  --------------------------------------------------
0       u8       version = 2
1       u8       ordinal strumienia wideo (0 dla naszego jednego)
2       u8       liczba rekordow (1..64)
3+10n   u16le    bazowy (brakujacy) RTP sequence dla rekordu n
5+10n   u64le    bitmapa base+1 .. base+64, modulo 65536
```

- `payload_length = 3 + 10 * record_count`
- **Każda baza jest niejawnie brakująca**; bit `j` żąda `base+1+j`
- Bieg 64 kolejnych braków = **jeden** rekord z 63 ustawionymi bitami, nie 64
- Pusta albo za duża partia → **odrzucona, nie przycinana**
- Maksymalna ramka: **647 B**
- Ponowienia: `clamp(floor(52 ms / RTT), 1, 3)` prób łącznie z pierwszą;
  odstęp `RTT + 4 ms`; przy `RTT = 0` → limit 3 i odstęp 4 ms
- Źródło RTT (w tej kolejności): wybrana para ICE → STUN wideo → STUN bundle.
  Próbki wygasają po **5 s**; bez świeżej → 30 ms (co pozwala na jedną próbę w budżecie 52 ms)
- Trasa Mjolnir **nie zależy** od otwarcia kanału RTCP

`MAX_NACK_FCI_ENTRIES = ceil(MAX_NACK_PACKET_COUNT / 17)` (`nvst.rs:208`).

### 5.3 Ścieżka odzyskiwania (z `docs/streamer-comparison/video.md`)

| Wyzwalacz | Akcja |
|---|---|
| Luka RTP w oknie NACK | Mjolnir: NACK-v2 `0x0317`; inne wideo: RTCP Generic NACK na SCTP `rtcp1`. Polling co **4 ms**. Każdy z ≤2048 oczekujących zachowuje **własny** deadline 52 ms i licznik prób. ≤64 pakiety/raport, ≤3 próby, backoff `RTT+4 ms`. Przepełnienie trackera → żądanie IDR |
| Nieodtwarzalna luka lub luka w numerze klatki nadawcy | `0x0301` dla utraconego zakresu klatek + **wstrzymanie zależnych obrazów**. Ponowienie po 100 ms, **bez przesuwania** backstopu IDR 250 ms. Czyszczenie na I / intra-refresh / RefPic-I. Wolny epizod nie wyłącza przyszłego RFI |
| Przepełnienie kolejki zakodowanej | Wyczyść łańcuch referencji, zażądaj klatki kluczowej |
| Błąd submit/poll dekodera | Przebuduj MFT na tym samym urządzeniu; jeśli nie — odtwórz Graphics+Decoder do 5 s |
| Timeout odtworzenia urządzenia | Fatalne, fallback klasowy |
| Luka pakietów → klatka kluczowa | **Nie zużywa** limitu prób odzyskiwania |

`reset_decoder` po utracie jest **wymagany**: sam `MFSampleExtension_CleanPoint` nie
zapomina uszkodzonych referencji H.264 (komentarz w `platform-windows/src/format.rs`).
NVST **nie wysyła FIR**.

---

## 6. Wejście i sterowanie — ramki NVB

### 6.1 Nagłówek ServerControl

```c
/* u16le command, u16le payload_length, potem payload */
0x0308 na drucie = 08 03 ...      /* LITTLE-endian! */
```

### 6.2 Sekwencja aktywacji sesji (dokładne bajty)

Z `docs/streamer-comparison/features.md`. **Kolejność jest istotna.**

| # | Komenda | Znaczenie | Payload (hex) |
|---|---|---|---|
| 1 | `0x020b` | enable input **false** | `0b 02 0c 00 00 00 00 00 01 00 00 00 00 00 00 00` |
| 2 | `0x020d` | device descriptor index 2 | `0d 02 30 00 23 00 00 00 00 01 32 bc 31 22 0c 00 00 00 1a 00 00 00 02 00 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 55 00 00 00 00 00 00 00 00 00 00 00 00` |
| 3 | `0x0308` | cursor capture **on** (NVB type 0) | `08 03 01 00 01` |
| 4 | `0x030d` | track remote cursor **on** (NVB type 8) | `0d 03 01 00 01` |
| 5 | `0x0320` | window state **19** | `20 03 0c 00 00 00 00 00 13 00 00 00 00 00 00 00` |
| 6 | `0x0321` | system state 0 | `21 03 0c 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| 7 | `0x020b` | enable input **true** | `0b 02 0c 00 00 00 00 00 01 00 00 00 01 00 00 00` |

Struktura payloadów:
- `0x020b` = **trzy `u32le`**: stream index (0), counter (1), enabled (0→1)
- `0x0320`/`0x0321` = **trzy `u32le`**: stream index (0), state, frame (0)
- `0x0308`/`0x030d` = jeden bajt payloadu: `0` off, `1` on

⚠️ **Window state musi wynosić 19** (`13 00 00 00`). Same zera sprawiają, że sesja wygląda
na nieaktywną i **serwer nie wysyła aktualizacji trybu kursora systemowego**. To jest
objaw, który wygląda jak „bug kursora", a jest błędem w jednej liczbie.

### 6.3 Pozostałe komendy

| Komenda | Znaczenie | Uwagi |
|---|---|---|
| `0x0200` | keepalive | wysyłany cyklicznie |
| `0x0206` | mysz relatywna | trasa: `control_channel_partially_reliable`. Test vector z `features.md`: dx=24, dy=24, timestamp `0x01861330` |
| `0x020e` | (version 3) | długość 2, wersja 3 |
| `0x0301` | utracony zakres klatek nadawcy | patrz §5.3 |
| `0x0302` | ponawiane co 100 ms aż do złożonej klatki kluczowej | |
| `0x0317` | NACK-v2 | patrz §5.2 |
| `0x0322` | **kursor** (nie haptics!) | wcześniejsze mapowanie na haptics było błędne |
| `0x0323` | rekonstrukcja NVB type 10 (mouse accel/speed) | **UNVERIFIED** — patrz niżej |
| `0x0206` inner type 13 | gamepad rumble (NVB type 6) | `u16le` flaga enable w kopercie z timestampem; poza naszym zakresem |

### 6.4 NVB type 10 — uczciwe ograniczenie

Oficjalny klient przy **każdej** zmianie fokusu wysyła `accel=0, speed=10` (NVB feature
type 10) i loguje `Disabling session mouse acceleration`. Ale
(`docs/streamer-comparison/features.md:123-125`):

> *„Searched and not present: `.pcap` / `.pcapng` / `.etl` under GFN AppData, Downloads,
> Documents. No control-frame hex next to type 10 in any `.log` / `.bak` … **There is no
> byte-exact type 10 frame to copy.** OpenNOW currently emits a reconstructed `0x0323` …
> That is not a capture."*

Czyli: **nikt nie ma dokładnych bajtów type 10**, OpenNOW też rekonstruuje. Robimy tak samo
i oznaczamy w kodzie `/* UNVERIFIED: reconstructed, no capture exists */`.

Znana jest natomiast treść logu błędu z `geronimo.log.bak`:
`Failed to update mouse sensitivity: 10 NVST_R_SUCCESS (0x0)` oraz
`nvbFeatureControl failed for type: 10` — gdzie `10` to wartość `speed` z formatu
`Failed to update mouse sensitivity: %d %s (0x%x)` w `Bifrost2.dll`.

### 6.5 Kanały danych SCTP

Etykiety (`nvst_input.rs:14-20`), profil 8 kanałów (`NVST_CHANNEL_PROFILE`):

```
control_channel_reliable                 Reliable
custom_channel_reliable                  Reliable
custom_channel_partially_reliable        MaxRetransmits(CUSTOM_PARTIAL_MAX_RETRANSMITS)
control_channel_partially_reliable       Lifetime(PARTIAL_RELIABLE_LIFETIME_MS)   <- mysz, NACK-v2
control_channel_unreliable               Unreliable
input_channel_partially_reliable         <- wejście
cursor_channel                           <- kursor
rtcp1                                    <- RTCP RR/PLI, gdy rtcpOnSctp=1
```

Rozbieżność do odnotowania (`docs/mjolnir-nack-v2.md`, koniec): *„OpenNOW's partial control
channel is unordered with a 300 ms lifetime. The inspected official default is ordered with
two SCTP retransmissions. Reusing the named channel matches routing, not that default
reliability policy."* Czyli nasza polityka niezawodności **nie jest** bajt-w-bajt jak
oficjalna — działa, ale to jest świadome odstępstwo (`x-nv-ri.partialReliableThresholdMs:300`
w ANNOUNCE).

---

## 7. Pułapki — lista zbiorcza (przeczytaj przed pisaniem kodu)

| # | Pułapka | Objaw, jeśli ją zignorujesz |
|---|---|---|
| 1 | Ciało GS jest **little-endian**, nagłówek RTP **big-endian** | wszystkie indeksy klatek są śmieciami |
| 2 | `fec_current_block`/`fec_last_block` siedzą w **`ext[11]`**, nie w osobnym polu | granice klatek nigdy nie trafiają |
| 3 | `words*4` musi być **dokładnie 16** | ciche przyjmowanie uszkodzonych rozszerzeń |
| 4 | Dla H.264 nagłówek GS znajduje się przez **szukanie start code w ≤68 B**, nie stałym offsetem | utracone lub nadmiarowe bajty na początku każdej klatki |
| 5 | Stałe offsety 8/20 są **tylko dla AV1** | jak wyżej, plus mylenie dwóch różnych układów GS |
| 6 | Końcowy **AUD (NAL 9) trzeba odciąć** | dekoder dostaje śmieci na końcu access unitu |
| 7 | `Ctr128BE` (KDF) ≠ `Ctr32BE` (dane) | cicha korupcja, wszystko „wygląda" poprawnie |
| 8 | `iv[7] ^= label`, a salt ma **14** B (CM) / **12** B (GCM) | każdy pakiet odrzucony |
| 9 | Do HMAC doklejamy **`be32(roc)`** | wszystkie pakiety odrzucone mimo dobrego klucza |
| 10 | Kolejność: unprotect → replay_check → payload → **commit na końcu** | pakiet z błędnym tagiem przesuwa okno replay (DoS) |
| 11 | `parity_shards = ceil(data*percent/100)` — **wyliczane**, nie przesyłane | FEC nigdy nic nie naprawia |
| 12 | **Macierz Cauchy'ego, nie Vandermonde'a** | *„mathematically valid but corrupt H.264 packets"* |
| 13 | `gf_inv(parity_shards + data_index XOR parity_index)` może **przekroczyć u8** | niezdefiniowane zachowanie przy dużych blokach |
| 14 | Część bajtów odtworzonego shardu to **metadane transportowe** | *„every real cloud repair was rejected"* mimo poprawnych danych |
| 15 | Kody komend NVB są **`u16le`** (`0x0308` → `08 03`) | serwer nie rozumie żadnej komendy |
| 16 | Window state **19**, nie 0 | serwer nie wysyła aktualizacji trybu kursora |
| 17 | Porównanie tagu **stałoczasowe** | side-channel (i przy okazji niestabilne odrzucenia) |
| 18 | `srtpAesKeyHex`/`srtpSaltHex` nigdy do logu | wyciek materiału kluczowego |
| 19 | Bufor gniazda 8 MiB → **zmniejsz do 1-2 MiB** na 2 GB RAM | presja na pamięć bez zysku na tetheringu |
| 20 | `-hwaccel dxva2` zwraca `rc=0` i **3-4× gorszy** wynik | myślisz, że masz dekoder sprzętowy (zmierzone w T1) |

---

## 8. Gdzie doczytać w źródłach OpenNOW

| Temat | Plik i linie |
|---|---|
| Stałe transportu | `nvst.rs:79-296` |
| Parsowanie RTP + rozszerzenie GS | `nvst.rs:2440-2515` |
| `NvVideoPacket::parse` | `nvst.rs:3217-3257` |
| Payload wideo / start code | `nvst.rs:3481-3495`; `find_annex_b_start_code` `nvst.rs:3525` |
| Kluczowa klatka | `nvst.rs:3500-3523` |
| Odcięcie końcowego AUD | `nvst.rs:3458-3479` (`strip_trailing_access_unit_delimiter`) |
| Profile SRTP i nazwy z SDP | `nvst.rs:301-329` |
| Materiał kluczowy z RTSP (hex) | `nvst.rs:1586-1640` |
| KDF | `nvst.rs:2873` (`derive_aes_cm_key`), `nvst.rs:2888` (`derive_aes128_cm_key`) |
| IV | `nvst.rs:2902` (`srtp_aes_cm_iv`), `nvst.rs:2915` (`srtp_gcm_iv`) |
| `unprotect` + kolejność replay | `nvst.rs:2635-2705` |
| AES-GCM z tagiem 8 B | `nvst.rs:2711-2790` |
| AES-CM + HMAC-SHA1 | `nvst.rs:2820-2872` |
| SRTCP | `nvst.rs:4357-4372` |
| FEC: layout bloku | `nvst.rs:3670-3701` |
| FEC: tablice GF(2^8) | `nvst.rs:3712-3742` |
| FEC: multiply/inverse/axpy/scale | `nvst.rs:3744-3783` |
| FEC: współczynnik Cauchy | `nvst.rs:3785-3793` |
| FEC: rekonstrukcja + metadane | `nvst.rs:3798-3880`, `4103-4106`, `4228-4248` |
| Reorder window | `nvst.rs:224-234`, `2238-2260` |
| Kanały SCTP | `nvst_input.rs:14-110` |
| RTSP + ANNOUNCE SDP | `opennow-streamer-core/src/nvst_rtsp.rs:1052-1180`, `1520-1620` |
| CloudMatch `POST v2/session` | `opennow-core/src/cloudmatch.rs:1643-1760` |
| Kolejka i statusy sesji | `cloudmatch.rs:2655-2708` |
| `seatSetupStep` (ekran kolejki) | `docs/session-start-progress.md` |
| Ramki NVB bajt po bajcie | `docs/streamer-comparison/features.md` |
| NACK-v2 | `docs/mjolnir-nack-v2.md` |
| Tor wideo i odzyskiwanie | `docs/streamer-comparison/video.md` |
| Audio: Opus/RED/jitter/WASAPI | `docs/streamer-comparison/audio.md` |
| Wzorzec IPC JSON-lines | `docs/core-protocol.md` |
| Raw Input (przenośny na C 1:1) | `opennow-streamer-platform/src/windows_raw_input.rs` |

Licencja OpenNOW: **MIT** (`LICENSE`, „Copyright (c) 2025 Zortos"). Nota musi zostać
w rozpowszechnianych kopiach.
