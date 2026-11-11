# Sepia kullanım kılavuzu

Sepia, gitarını ses kartına takıp amfi, kabin ve pedal sesleriyle çalmanı sağlayan ücretsiz bir
programdır. Gerçek amfi, mikrofon ya da pedal gerekmez.

## 1. Bağlantı

1. Gitarı ses kartının **Instrument / Hi-Z** girişine tak. Normal mikrofon ya da line girişinde
   ses ince ve cansız olur.
2. Kulaklığı ya da hoparlörleri ses kartına bağla (bilgisayarın kendi çıkışına değil).
3. Sepia'yı aç, sağ üstteki çark simgesinden ses ayarlarına gir:
   - **Sürücü:** ses kartının kendi ASIO sürücüsü.
   - **Örnekleme hızı:** 48 kHz.
   - **Buffer:** 128 sample ile başla. Çıtırtı duyarsan 256 yap; gecikme hissedersen 64 dene.
4. Ses kartındaki giriş (gain) düğmesini, en sert vuruşunda giriş göstergesi kırmızıya girmeyecek
   kadar aç.

## 2. Preset seçmek

Ortadaki preset adına tıkla. Hazır tonlar "Sepia Factory" altında altı grupta durur:

| Grup | Ne için |
|---|---|
| Clean & Indie | Temiz ve hafif kırılan tonlar, chorus, tremolo, akustik benzeri sesler |
| Lo-Fi | Kaset, VHS, plak, radyo, telefon, 8-bit |
| Shoegaze & Ambient | Büyük reverb'ler, fuzz duvarları, pad'ler |
| Blues & Classic Rock | Kırılan amfiler, overdrive, wah, solo tonları |
| Grunge & Punk | Distortion, fuzz, sert ritim tonları |
| Metal | Yüksek gain ritim ve lead tonları |

Preset adının iki yanındaki oklarla sırayla gezebilirsin. Kalp simgesi preset'i favorilere ekler.
Bütün hazır tonlar aynı ses yüksekliğine ayarlıdır; birinden diğerine geçince ses sıçramaz.

## 3. Zincir: amfi, kabin ve pedallar

Preset'in altındaki kutular sesin geçtiği yolu gösterir: soldan sağa **Input → pedallar → amfi →
kabin → efektler → Output**. Bir kutuya tıklayınca düğmeleri altta açılır.

- **Pedal eklemek:** iki kutunun arasındaki küçük **+** işaretine ya da sağdaki **+** düğmesine
  bas, listeden seç.
- **Sırayı değiştirmek:** kutuyu sürükle.
- **Kapatıp açmak:** kutunun düğme panelindeki ON anahtarı.
- **Silmek:** kutunun üstüne gelince çıkan **×**.

Genel kural: gate, kompresör, wah, overdrive, distortion, fuzz ve Sustainer **amfiden önce**;
chorus, delay, reverb, Tape, Vinyl ve Bitcrusher **kabinden sonra** durur.

### Amfi karakterleri

Zincirdeki amfiye ("Heavy American") tıkla, sağ üstteki **Presets** menüsünü aç. On altı hazır
amfi karakteri var:

| Temiz | Kırılan | Crunch | Yüksek gain |
|---|---|---|---|
| Clean Channel | Edge of Breakup | Classic Crunch | Tight Modern Rhythm |
| Glass Clean | Chime Combo | British Crunch | Tight Djent |
| Warm Jazz | Tweed Breakup | | Scooped Thrash |
| | | | Vintage High Gain |
| | | | Hot-Rodded Stack |
| | | | Doom Wall |
| | | | Singing Lead |
| | | | Liquid Lead |

Bir karakter seçtikten sonra Gain, Bass, Middle, Treble düğmeleriyle istediğin gibi ayarlarsın.

### Pedal türleri

| Tür | Pedallar |
|---|---|
| Drive | Overdrive (7 model), Distortion (4 model), Fuzz (5 model), Multiband Distortion |
| Dinamik | Noise Gate, kompresörler, limiter, gürültü azaltma, **Sustainer** |
| Modülasyon | Chorus, Flanger, Phaser, Tremolo, Vibe, Rotary, Wah, Ring Mod, **Vowel Filter** |
| Perde | Octave, Pitch Shift, Harmonizer, Transpose |
| Zaman | Dijital, analog ve bant delay'leri, doubler, **Stutter**, altı ayrı reverb |
| EQ | Parametrik ve grafik EQ, Pickup Match, **Acoustic** |
| Lo-fi | **Tape**, **Vinyl**, **Bitcrusher**; kabinde AM Radio, Telephone, Boombox, Practice Amp, VHS |

Kalın yazılanlar Sepia'ya özgüdür.

## 4. Sepia'ya özgü pedallar

**Sustainer** (amfiden önce). Tuttuğun notayı yaklaşık bir saniye sonra feedback gibi kabartır ve
tel söndükten sonra da sürdürür. Yüksek sesle çalan gerçek bir amfinin odada yaptığı şeyin
kulaklıktaki karşılığıdır.
- *Amount:* eklenen sesin gücü.
- *Bloom:* devreye girmeden önce notayı ne kadar tutman gerektiği.
- *Hold:* tel söndükten sonra kaç saniye süreceği.
- *Harmonic:* aynı nota, bir oktav üstü ya da daha yukarısı.
- Yeni bir notaya vurunca eski ses kesilir. Hızlı çalarken devreye girmez.
- Uzun tutulan bir akorda akorun kök sesini öne çıkarabilir (gerçek amfideki feedback gibi).
  Bunu istemiyorsan Bloom'u uzat ya da akor çalarken pedalı kapat.

**Tape** (kabinden sonra). Kaset ve video bandı karakteri: *Drive* doygunluk, *Wow* ve *Flutter*
perde dalgalanması, *Hiss* tıslama, *Dropout* anlık ses düşmeleri, *Tone* üst uç kaybı.

**Vinyl** (zincirin sonunda). *Crackle* plak çıtırtısı, *Dust* sürekli hışırtı, *Wear* yıpranmış
plağın donuk üst ucu.

**Bitcrusher** (kabinden sonra). *Bits* düştükçe ses kabalaşır, *Downsample* arttıkça metalik
çınlama gelir. Eski oyun konsolu ve sampler sesi.

**Acoustic** (amfi yerine ya da temiz amfiden sonra). Elektro gitarı akustik gitara yaklaştırır:
*Size* gövde boyu, *Body* gövde tınısı, *Top* parlaklık. Boyun manyetiğiyle en iyi sonucu verir.
Bir EQ'dur; gerçek akustik gitarın yerini tutmaz.

**Stutter** (kabinden sonra). Az önce çaldığın parçayı yakalayıp tempoya uygun biçimde tekrarlar;
"takılan CD" etkisi. *Division* tekrar uzunluğu, *Chance* ne sıklıkla devreye gireceği, *Repeats*
kaç kez tekrarlayacağı, *Reverse* tersten çalma olasılığı. Tempo alt çubuktaki BPM'den gelir.

**Vowel Filter** (amfiden önce ya da sonra). Gitarı "u-o-a-e-i" diye konuşturan filtre; talk box
ve wah arası bir ses. *Mode*: elle (Manual), kendiliğinden salınan (LFO) ya da vuruş gücüne göre
açılan (Envelope). *Vowel* ağzın konumu, *Depth* ne kadar hareket edeceği, *Resonance* keskinlik.

## 5. Sık sorulanlar

**Ses gecikmeli geliyor.** Ses ayarlarında ASIO sürücüsünün seçili olduğundan emin ol ve buffer'ı
küçült (128, sonra 64).

**Çıtırtı ve kesilme var.** Buffer'ı büyüt (256). Başka programları kapat. Çok ağır bir preset
(büyük reverb'ler) eski bilgisayarı zorlayabilir.

**Çalmadığımda uğultu var.** Yüksek gain'li tonlarda normaldir. Sol üstteki **GATE** anahtarını aç
ve eşiği uğultu kesilene kadar yükselt. Tek bobinli manyetiklerde bilgisayar ekranından uzaklaş.

**Notalar hemen sönüyor.** Önce teli perdeye sağlam bastığından ve tellerin eski olmadığından emin
ol. Sonra "Singing Sustain", "Endless Solo" gibi bir lead preset'i dene ya da zincire Sustainer
ekle.

**Kendi amfi modelimi yükleyebilir miyim?** Evet. Zincire "Neural Amp (NAM)" ekleyip `.nam` dosyanı, kabin
için "IR Cabinet" ekleyip `.wav` impulse response dosyanı seçebilirsin. Hazır tonların hiçbiri
bunlara ihtiyaç duymaz.

**İnternet gerekiyor mu?** Hayır. Sepia çevrimdışı çalışır; çevrimiçi preset paylaşımı ve güncelleme
denetimi yoktur.

## 6. Görünüm

Sağ alttaki listeden tema seçilir: **Sepia** (varsayılan), Light, Dark, Vintage.
