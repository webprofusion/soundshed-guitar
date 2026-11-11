# Sepia'da neler var

Sepia, açık kaynak Soundshed Guitar'ın bir fork'udur. Ses motoru, dahili amfi ve kabin modeli,
elliden fazla efekt, tuner, looper ve metronom oradan gelir. Aşağıdakiler Sepia'nın eklediği ya
da değiştirdiği şeylerdir.

## Yeni pedallar

| Pedal | Ne yapar | Zincirde yeri |
|---|---|---|
| **Tape** | Kaset/VHS doygunluğu, perde dalgalanması, tıslama, dropout | Kabinden sonra |
| **Vinyl** | Plak çıtırtısı, hışırtı, yıpranmış üst uç | En sonda |
| **Bitcrusher** | Düşük bit ve örnekleme hızı; 8-bit, eski sampler | Kabinden sonra |
| **Sustainer** | Tutulan notayı feedback gibi kabartır ve tel sönse de sürdürür | Amfiden önce |
| **Acoustic** | Elektro gitarı akustik gitara yaklaştırır | Amfi yerine |
| **Stutter** | Son çalınan parçayı tempoya uygun tekrarlar, bazen tersten | Kabinden sonra |
| **Vowel Filter** | Gitarı "u-o-a-e-i" diye konuşturur; elle, LFO ile ya da vuruş gücüyle | Amfiden önce ya da sonra |

Her pedalın kendi görseli vardır; panelinde ve zincirdeki kutusunda görünür.

## Yeni kabinler ve amfi karakterleri

- **Beş lo-fi kabin:** AM Radio, Telephone, Boombox, Practice Amp, VHS.
- **Sekiz amfi karakteri daha** (toplam on altı): Glass Clean, Warm Jazz, Chime Combo, Tweed
  Breakup, British Crunch, Hot-Rodded Stack, Doom Wall, Liquid Lead.

## Hazır tonlar

Altı grupta 136 preset: Clean & Indie, Lo-Fi, Shoegaze & Ambient, Blues & Classic Rock,
Grunge & Punk, Metal.

- Hiçbiri ek dosya (amfi capture'ı, IR) istemez; kurar kurmaz çalar.
- Hepsi aynı ses yüksekliğine ayarlıdır ve preset değiştirirken tık sesi çıkmaz.
- Tarayıcıdaki etiket düğmeleri (clean, crunch, high-gain, lead, rhythm, ambient) bunları süzer;
  clean/crunch/high-gain etiketi her preset'in ölçülen gain miktarından gelir.

## Görünüm

- **Sepia teması** (varsayılan): koyu kahve zemin, krem yazı, altın vurgu. Light, Dark ve
  Vintage temaları da seçilebilir.
- Kendi uygulama simgesi ve pedal görselleri.

## Bilerek çıkarılanlar

- **Çevrimiçi özellikler:** preset paylaşımı, güncelleme denetimi, YouTube'da backing track
  arama. Bunlar Soundshed'in kendi sunucularını kullanıyordu; fork'ların kullanmasına izin
  yok. Sepia hiçbir sunucuya bağlanmaz.
- **Soundshed'in hazır preset paketi:** başkalarına ait amfi capture'ları içeriyordu.

Kendi NAM modellerini ve IR dosyalarını yüklemek hâlâ mümkün.

## Neyin sınandığı, neyin sınanmadığı

Otomatik testler ölçülebilen şeyleri sınar: seviyeler, kırpma, gain yapısı, frekans tepkileri,
preset geçişlerinde tık, işlemci yükü, bozuk girişe dayanıklılık. Arayüz, gerçek motora bağlı
olarak bir tarayıcıda açılıp ekran görüntüleriyle kontrol edildi.

Sınanamayan tek şey en önemlisi: **tonların kulağa nasıl geldiği.** Preset'ler sentezlenmiş gitar
kayıtlarıyla ayarlandı; gerçek gitarla dinlenip düzeltilmeleri gerekir. Nasıl deneneceği
`sepia-testing.md` dosyasında, nasıl kullanılacağı `sepia-kullanim.md` dosyasında yazıyor.
