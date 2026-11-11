# Sepia test rehberi

Otomatik testler ses seviyesini, kırpmayı, gain yapısını ve preset geçişindeki tıklamayı ölçer.
Ölçemedikleri şeyler bu listede: gerçek gitar, gerçek ses kartı ve kulak gerektirir.

Her adımda sorun varsa **hangi preset**, **ne yaptın**, **ne duydun** yazman yeterli.

## 1. Kurulum (Windows)

1. GitHub'da fork'un **Actions** sekmesinden son başarılı "Build Windows" çalışmasını aç ve
   `windows-x64-installer` dosyasını indir.
2. Bilgisayarın internet bağlantısını kapat ve kurulumu çalıştır.
3. **Sepia** uygulamasını aç.

Kontrol et:

- [ ] Kurulum internetsiz tamamlandı.
- [ ] Uygulama açıldı ve başlıkta "Sepia" yazıyor.
- [ ] Açılışta "Warm Clean Room" preset'i yüklü.
- [ ] Preset listesinde "Sepia Factory" altında altı grup ve toplam 120 preset var.
- [ ] Uygulama sıcak kahverengi-altın "Sepia" temasıyla açılıyor (Ayarlar'dan Light, Dark, Vintage seçilebiliyor).
- [ ] Hiçbir preset "eksik dosya" ya da "model bulunamadı" uyarısı vermiyor.
- [ ] Üst çubukta paylaşım (Tones) sekmesi yok, güncelleme bildirimi çıkmıyor.

## 2. Ses kartı ayarı

1. Ayarlardan ses kartının kendi **ASIO** sürücüsünü seç.
2. Buffer'ı 128 sample yap, örnekleme hızını 48 kHz.
3. Gitarı takıp giriş seviyesini ayarla: en sert vuruşta kırmızıya girmesin.

Kontrol et (64, 128 ve 256 buffer için ayrı ayrı):

- [ ] Ses kesilmiyor, çıtırtı yok.
- [ ] Çalarken gecikme hissedilmiyor (64 ve 128'de).
- [ ] Performans göstergesindeki CPU yüzdesini not et: temiz bir preset ve "Djent Machine" için.

## 3. Preset'ler

Her grubu sırayla gez. Her preset için:

- [ ] Ses geliyor ve diğer preset'lerle yaklaşık aynı yükseklikte.
- [ ] Preset değiştirirken tık ya da pat sesi yok.
- [ ] Adı tonla uyumlu (örnek: "Kitchen Radio" gerçekten radyo gibi dar ve cızırtılı).
- [ ] Metal grubunda: tellere dokunmadığında sessiz, palm mute sıkı.

Özellikle dinlenmesi gerekenler, çünkü ölçümle değerlendirilemiyor:

| Preset | Bakılacak şey |
|---|---|
| Camcorder Summer, Worn Out Mixtape | Perde dalgalanması hoş mu, mide bulandırıyor mu? |
| Kitchen Radio, Answering Machine | Çok mu ince, çalınabilir mi? |
| Eight Bit Garage, Dusty Sampler | Bitcrusher karakter mi katıyor, yalnızca gürültü mü? |
| Tight Modern Rhythm, Djent Machine, Metalcore Chug | Düşük teller çamurlu mu, sıkı mı? |
| Wall of Haze, Fuzz Blanket | Akorlar seçiliyor mu, tamamen bulanık mı? |
| Singing Metal Lead, Shred Legato | Tek notalar uzuyor mu, tiz mi, yumuşak mı? |
| Endless Solo, Slide Sustain, Feedback Garden | Sustainer notayı doğal biçimde uzatıyor mu, yapay bir "bip" gibi mi duruyor? |
| Feedback Squeal, Static Bloom | Üst harmoniğe dönen feedback hoş mu, rahatsız edici mi? |
| Vinyl Sunday, Charity Shop LP, Run-Out Groove | Çıtırtı plak gibi mi, yoksa arıza gibi mi? |
| Auto Wah Funk, Wah Riot | Wah vuruşa tepki veriyor mu? |

## 4. Yeni efektler

Boş bir zincire tek tek ekle:

- [ ] **Tape**: Wow ve Flutter sıfırken perde sabit; yükseltince dalgalanıyor. Hiss açıkken
      hafif bir tıslama var, sıfırda yok. Yankı duyulmuyor.
- [ ] **Bitcrusher**: Bits düştükçe ses kabalaşıyor; Downsample arttıkça metalik çınlama geliyor.
- [ ] **Sustainer** (amfiden önce): bir notayı basılı tut; yaklaşık bir saniye sonra nota
      "feedback" gibi kabarıp tel söndükten sonra da sürüyor. Yeni bir notaya vurunca eski ses
      hemen kesiliyor. Hızlı çalarken hiçbir şey eklemiyor. Akorlarda devreye girmemesi normal.
- [ ] **Vinyl** (zincirin sonunda): Crackle ile çıtırtı, Dust ile hafif hışırtı geliyor; ikisi
      sıfırken ses temiz.
- [ ] **Simple Cabinet**: kabin listesinde AM Radio, Telephone, Boombox, Practice Amp ve VHS var
      ve her biri belirgin biçimde farklı.

## 5. VST3 (DAW kullanırsan)

- [ ] Reaper'da (ya da başka bir DAW'da) plugin listesinde "Sepia" görünüyor.
- [ ] Projeyi kaydedip yeniden açınca aynı preset ve ayarlar geri geliyor.

## Bilinen sınırlar

- Windows kurulum dosyası imzasız: Windows "bilinmeyen yayıncı" uyarısı gösterir.
- Tonlar sentezlenmiş gitar kayıtlarıyla seviyelendi; gerçek gitarında, özellikle manyetiğin
  çıkışına göre, biraz farklı yükseklikte duyulabilir.
- Soundshed'in çevrimiçi özellikleri (preset paylaşımı, güncelleme denetimi) bilerek kapalı.
