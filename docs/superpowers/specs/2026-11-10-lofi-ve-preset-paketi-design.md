# Soundshed fork'u: lo-fi efektler ve geniş preset paketi — tasarım

Tarih: 2026-11-10

## Amaç

Ücretsiz ve açık kaynak bir gitar amfi sim'i. Gitarı ses kartına takıp bir preset seçince, başka bir şey indirmeden iyi ses versin. Paralı sim'lere (29–100 $) para vermemek için kişisel kullanım; isteyen herkes de kullanabilsin.

Başarı ölçütleri:
- Temiz kurulumdan sonra, internet bağlantısı olmadan, indie/lo-fi'den metale kadar her türde hazır tonlar listede görünür ve çalar.
- Hiçbir fabrika preset'i ek `.nam`, IR ya da üçüncü taraf plugin dosyası istemez.
- Preset değişiminde tık/pat sesi ve belirgin seviye sıçraması olmaz.
- Gerçek bilgisayarda ve ses kartında normal çalma sırasında ses kesilmez; CPU yükü ve uçtan uca gecikme ölçülür (kullanıcı tarafından, Windows'ta).

Referans: Roomtone'un indie/bedroom pop/lo-fi odağı ve "preset seç, çal" deneyimi. Roomtone'un kodu, arayüzü, preset adları ya da ayarları kullanılmaz; DSP ve preset'ler özgündür.

## Yaklaşım

Soundshed Guitar'ı (AGPL-3.0) fork'layıp üstüne ekliyoruz. Sıfırdan yazmıyoruz ve NAM depolarını ayrıca birleştirmiyoruz, çünkü:

- Soundshed NAM çekirdeğini zaten içeriyor.
- Kendi analog amfi modeli (`BuiltinAmp`, temiz ve drive kanalı), 5 tipli kabin modeli (`SimpleCab`), 50'den fazla efekt, tuner, looper, Windows ve Linux derlemesi hazır.

Sonuç AGPL-3.0 kalır: ücretsiz, kaynak kodu açık.

## Kapsam

### 0. Yeniden markalama ve ağ bağlantısının kaldırılması
Soundshed README'si açıkça belirtiyor: Soundshed/Soundshed Guitar markasını kullanma izni verilmiyor ve uygulamanın kullandığı servisler/API'ler yalnızca kendi yazılımları için. Bu yüzden fork:

- Yeni ürün adı, üretici adı, alan adı ve plugin kimlikleriyle derlenir (`core/config/GuitarFXConfig.h`; `PLUG_UNIQUE_ID`, `PLUG_MFR_ID`, `BUNDLE_DOMAIN` dahil). Aynı makinede asıl Soundshed ile çakışmaz. Logo ve marka görselleri değiştirilir.
- `api-guitar.soundshed.com` adresine hiçbir istek atmaz: preset paylaşımı, proxy'li kaynak arama ve YouTube gömme özellikleri kapatılır ve arayüzde gizlenir.
- Varsayılan açılış preset'i (`GUITARFX_DEFAULT_STARTUP_PRESET_TITLE`) kendi paketimizden bir preset olur.
- Telif ve lisans bildirimleri (LICENSE, THIRD_PARTY_NOTICES, asıl projeye atıf) korunur.

TONE3000'in resmi API'si Soundshed'e ait değildir; proxy'siz çalışıyorsa kalabilir, çalışmıyorsa o da kapatılır. Uygulama planında koda bakılarak kesinleşir.

### 1. Lo-fi kabin tipleri
`SimpleCab`'in kabin listesine (şu an 5 tip) 5 yeni tip: AM radyo, telefon, boombox, küçük pratik amfi, VHS. Her biri mevcut kabin kalıbında bir filtre voicing'i: dar bant, belirgin rezonans, sınırlı üst uç. VHS tipi yalnızca frekans karakterini verir; gerçek VHS/kaset hissi (dalgalanma, doygunluk, hiss, dropout) 2. maddedeki tape efektinden gelir ve preset'lerde ikisi birlikte kullanılır. Mevcut 5 tipin sesi ve varsayılanlar değişmez; eski preset'ler aynı çalar.

### 2. Tape/VHS saturation efekti (yeni)
Parametreler: drive, wow (yavaş perde dalgalanması), flutter (hızlı dalgalanma), hiss, dropout (anlık seviye düşmeleri), ton (üst uç kaybı), mix. Kaset ve VHS ayrı motor değil, bu efektin fabrika preset'leridir.

Tape Echo'dan bağımsız bir efekttir ve yankı üretmez. Perde dalgalanması için mevcut `TapeTransport` mantığı yeniden kullanılır. Wow/flutter doğası gereği kısa bir modülasyonlu gecikme hattı gerektirir; bu gecikme modülasyon derinliğinin gerektirdiği en düşük değerde tutulur (hedef 5 ms'nin altında) ve wow ile flutter sıfırken hat devre dışı kalır. Doygunluk oversampling ile çalışır.

### 3. Bitcrusher efekti (yeni)
Parametreler: bit derinliği, örnekleme hızı düşürme, mix. Örnekleme düşürmenin ürettiği aliasing efektin karakteridir, bastırılmaz.

### 4. Preset paketi
Yaklaşık 60 preset, her türden:

| Grup | Adet (yaklaşık) |
|---|---|
| Temiz, indie, bedroom pop, dream pop | 12 |
| Lo-fi (radyo, VHS, bant) | 8 |
| Shoegaze, ambient | 8 |
| Blues, klasik rock, crunch | 10 |
| Grunge, alternatif, punk | 10 |
| Metal (ritim, lead, modern, eski okul) | 12 |

Kurallar:
- Yalnızca dahili amfi, dahili kabin ve dahili efektler kullanılır. NAM capture'ı ya da IR dosyası gerektirmez; capture'ların lisansları dağıtmaya uygun olmayabilir.
- Adlar özgündür ve tonu tarif eder. Sanatçı, şarkı ya da başka ürünlerin preset adları kullanılmaz.
- Seviyeler eşitlenir: preset değiştirince ses sıçramaz.
- Yüksek gain preset'lerinde noise gate açık gelir.

## Kapsam dışı

- Yeni arayüz ya da görsel yeniden tasarım. Yeni efektler mevcut efekt listesinde, mevcut genel panel ile görünür.
- Yeni amfi modeli. Metal ve grunge tonları mevcut drive kanalı + overdrive/distortion/fuzz + EQ ile kurulur.
- macOS, AAX, mobil derlemeler.
- Sanatçı adına göre arama.
- Vinyl crackle. İlk sürümde yok; dinleme testlerinden sonra istenirse eklenir.
- Çevrimiçi preset paylaşımı.

## Mimari uyum

- Her yeni efekt Soundshed kalıbını izler: `EffectGuids.h` içinde kalıcı UUID, `EffectRegistry` kaydı, parametre tablosu, `FactoryPresetSupport` ile efekt içi preset'ler.
- Ses iş parçacığında bellek ayırma ve kilit yok; sonlu olmayan (NaN/Inf) girişe karşı mevcut koruma kalıbı uygulanır.
- Dosya boyutu sınırı: yeni dosyalar 800 satırın altında (`tools/check-cpp-file-sizes.js`).
- Preset'ler mevcut preset şeması ve fabrika preset mekanizmasıyla eklenir.

## Test

Her birim için önce test, sonra kod:

- Lo-fi kabinler: her tipin frekans tepkisi beklenen bantta; mevcut 5 tipin tepkisi bit düzeyinde değişmemiş.
- Tape: wow/flutter kapalıyken perde sabit ve ek gecikme yok; drive ile harmonik artar; doygunluktan gelen istenmeyen aliasing sınırın altında; hiss ve dropout kapalıyken sessiz girişte çıkış sessiz; yankı üretmez.
- Bitcrusher: bit derinliği beklenen niceleme adımını verir; mix=0 girişi aynen geçirir. Aliasing ölçülmez, çünkü istenen davranıştır.
- Tüm yeni efektler: NaN/Inf girişte çıkış sonlu; mevcut fabrika preset test paketi (`EffectFactoryPresetTests`) geçer.
- Preset'ler: hepsi yüklenir, eksik kaynak yok, yüksek gain adlı olanlar gerçekten yüksek gain. Ses yüksekliği en az üç farklı DI kaydıyla ölçülür (temiz akor, palm mute ritim, lead); her birinde ortak hedefin ±1 dB içinde kalır ve çıkış dijital kırpmaya (0 dBFS) ulaşmaz.
- Ağ: derlenmiş uygulamada `api-guitar.soundshed.com` adresine giden kod yolu kalmadığı kaynak taramasıyla doğrulanır.
- Her DSP değişikliğinden sonra mevcut test paketi ve ses anlık görüntü testleri (`AudioSnapshot`, `tools/audio-ab`) çalıştırılır; mevcut efektlerin çıkışı değişmemiş olmalıdır.

Ölçümle doğrulanamayan şey: tonun kulağa iyi gelip gelmediği. Bu, gitar geldikten sonra kullanıcı tarafından dinlenip ayarlanır.

## Derleme ve dağıtım

- Linux: bulut oturumunda derlenir ve testler çalıştırılır.
- Windows: fork'taki mevcut GitHub Actions iş akışı (`build-windows.yml`) kurulum dosyasını üretir.
- Hedef formatlar: Standalone ve VST3.

## Geliştirme sırası

1. Temel doğrulama: fork'un sabit bir commit'inde hiçbir şey değiştirmeden derleme ve mevcut testler. Sonraki her adım bu sonuçla karşılaştırılır.
2. Yeniden markalama ve ağ özelliklerinin kapatılması.
3. Beş lo-fi kabin.
4. Tape/VHS efekti, ardından bitcrusher.
5. Preset'ler: önce her gruptan iki referans ton, ölçüm; sonra kalanı.
6. Windows Standalone + VST3 derlemesi; temiz Windows'ta internetsiz kurulum ve gerçek ses kartıyla deneme.

Her adım ayrı commit'lerdir.

## Ön koşul

Fork hazır: `justsomeone-e/soundshed-guitar`. Bu oturumun fork'a yazabilmesi için Claude GitHub uygulamasının fork'a kurulması gerekir. Ürün adı kullanıcı tarafından seçilir. Bu doküman fork'a `docs/superpowers/specs/` altına taşınıp commit'lenir.

## Riskler

- Dahili drive kanalı modern metal için yeterince sıkı olmayabilir. Öyleyse önüne overdrive ve EQ konur; yine yetmezse yeni amfi voicing'i ayrı bir iş olarak ele alınır.
- Fork'ta GitHub Actions'ın Windows derlemesi imzalama anahtarı ya da gizli değişken isteyebilir; iş akışı fork'ta ilk çalıştırıldığında netleşir.
- Asıl proje çok aktif (günlük commit). Fork geride kalabilir; değişiklikler küçük ve ayrı dosyalarda tutulur ki güncelleme kolay olsun.
