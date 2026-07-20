# Paths Parser

`.txt` dosyalarındaki dosya yollarını (paths) ve belleği tarayarak analiz sonuçları çıkarır.

## Ne İşe Yarar?
- `.txt` dosyalarındaki dosya yollarını **ayrıştırır (parse)**.
  - Örneğin, `0x1ffd7017eb0 (109): \??\C:\users\user\desktop\cheat.dll` gibi bir satır `C:\users\user\desktop\cheat.dll` olarak yakalanır.
- Her dosya için **dijital imza (digital signature)** kontrolü yapar:
  - **Catalog** ve **Authenticode** imzalarını kontrol eder.   
  - Eğer dosya bilgisayarda yoksa **"Silinmiş"** (Deleted) olarak raporlar.
  - Belirli hile yazılımlarının dijital imzalarını (örn. Slinky, Vape gibi bilinen hileler) tespit eder.
  - Dosya mevcutsa **"Geçerli İmza"** veya **"İmzasız"** olarak raporlar.
- Bulunan dosyalara karşı **genel tarama kurallarını (Generic Rules)** uygular (aşağıda detayları var).
- USN journal'ı kullanarak her dosya için (Explorer, Copy veya Type kopyalamaları) **"Replace"** (Değiştirilme) durumlarını kontrol eder.

## Nasıl Kullanılır?

1. **Hedef yollarınızı şu dosyalardan birine veya birkaçına ekleyin** (Programla aynı dizinde veya `C:\` dizininde olabilir):
   - `search results.txt`
   - `paths.txt`
   - `p.txt`

   Bu dosyalar istediğiniz kadar satır içerebilir. İçinde `C:\klasor\dosya.exe` gibi yollar bulunan satırlar otomatik olarak ayrıştırılacaktır. Geçersiz girişler veya düzgün bir dosya yolu içermeyen satırlar göz ardı edilir.

2. **Derlenmiş programı çalıştırın** (`PathsParser.exe`).  

3. **Yeni Grafiksel Kullanıcı Arayüzünü (GUI) Kullanın:**
   - Üst kısımdaki çubuktan tarama tercihlerinizi kutucuklarla ayarlayabilirsiniz:
     - **TARA**: Tarama işlemini başlatır.
     - **YARA**: Gömülü genel YARA kurallarıyla taramayı açıp kapatır.
     - **Özel YARA**: Kendi `.yar` dosyalarınızla tarama yapmanızı sağlar.
     - **Replace**: USN journal üzerinden dosyaların kopyalanıp kopyalanmadığını denetler.
     - **Sadece DLL**: Sonuçları yalnızca DLL dosyalarını gösterecek şekilde filtreler.
     - **Bellek Taramaları**: CSRSS, Explorer veya AppInfo'yu doğrudan sistem belleğinden tarar.
   
4. **Sonuçları Analiz Edin:**
   - Etkileşimli tablo size şunları gösterecektir:
     - Dosya yolları (orijinal Windows simgeleriyle birlikte).
     - Dosyanın **Mevcut** mu yoksa **Silinmiş** mi olduğu durumu.  
     - **Dijital imza** durumu.  
     - Herhangi bir **YARA** kuralı eşleşmesi.
     - Herhangi bir **Replace** tespiti.
   - "Ara" çubuğunu kullanarak yüzlerce sonuç arasında hızlıca arama yapabilirsiniz.
   - Tablodaki herhangi bir yazıya **`CTRL` + Tıklama** yaparak yazıyı panoya kopyalayabilirsiniz.

5. Tarama bittiğinde program:
   - Bulunan tüm "Replace" sonuçlarını programla aynı klasördeki `replaces.txt` dosyasına yazar.

### Özel YARA Kuralları (Custom YARA) Kullanımı

Kendi kurallarınızı kullanmak için `.yar` uzantılı dosyalarınızı `PathsParser.exe` ile aynı klasöre koymanız yeterlidir. Daha sonra arayüzdeki **Özel YARA** kutucuğunu işaretleyin. Program tarama sırasında kurallarınızı otomatik olarak yükleyecek, derleyecek ve uygulayacaktır.

## YARA Kuralları (Generics)

1. **Generic A**: Autoclicker'lar için temel kelimeler  
2. **Generic A2**: Autoclicker'lar için import kombinasyonları  
3. **Generic A3**: C# tabanlı autoclicker'lar için genel tespit  
4. **Generic B - B7**: C# tabanlı olmayan dosyalar için genel koruma (protection) tespitleri  
5. **Generic C**: C# dosyaları için temel koruma tespiti  
6. **Generic D**: C# dosyaları için gelişmiş koruma tespiti  
7. **Generic E**: C# ve derlenmiş Python dosyaları için temel koruma tespiti  
8. **Generic F - F5**: Paketlenmiş (Packed) çalıştırılabilir dosyalar için gelişmiş genel tespitler  
9. **Generic F6**: **Çok fazla** paketlenmiş çalıştırılabilir dosyalar için tespit  
10. **Generic F7**: **AŞIRI (SUPER)** paketlenmiş çalıştırılabilir dosyalar için tespit  
11. **Generic G - G4**: Şüpheli injector uygulamaları için gelişmiş genel tespitler  
12. **Specific A**: Basit string'ler (kelimeler) ile bazı ücretsiz hileleri tespit eder  
13. **Specific A2**: Çoğu bilinen DLL clicker'larını string analiziyle tespit eder  
14. **Specific B**: Gelişmiş yöntemlerle bazı ücretli (paid) hileleri tespit eder  

> **Not:** A2 ve F sınıfı kurallar **bazen yanlış uyarı (false positive)** verebilir, ancak asıl hileleri kaçırmamak adına sistemde tutulmaktadır.
