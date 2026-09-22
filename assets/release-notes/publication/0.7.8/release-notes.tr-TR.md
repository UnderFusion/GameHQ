# GameHQ 0.7.8 (2026-09-22)

## Öne çıkanlar

- Oyun içi arayüz artık kenarlıksız oyunların üzerinde çok daha güvenilir çalışıyor. Oyunun üzerinde görünür, desteklenen GameInput yollarında oyun içi arayüzde gezinmek için kumanda girişini alır ve oyun görünür kalmaya devam eder. Indiana Jones and the Great Circle oyununda, kenarlıksız modda kablolu bir DualSense ile doğrulanmıştır.
- Desteklenen GameInput yollarında oyun içi arayüzde gezinmek artık arkadaki oyunu kontrol etmiyor. Oyun içi arayüz kapatılırken kumanda, basılı tuttuğunuz tüm düğmeleri bırakmanız beklendikten sonra oyuna geri verilir.
- Anında tekrar klipleri artık daha güvenli: art arda hızlı kayıtlar mevcut bir klibin üzerine asla yazmaz ve kayıt başlar başlamaz klip kaydedilebilir.
- Yeni eşleme ön ayarları: kumanda düzenlerini oluşturup düzenleyebilir, her kumanda ve her oyun için ayrı ayrı atayabilirsiniz.
- GameHQ kaldığınız yeri hatırlar: son sayfa, Ayarlar kategorisi, galeri filtresi ve her oyunun oyun içi arayüz kategorisi.

## Oyun içi arayüz

- Oyun içi arayüz, görünür durumdaki kenarlıksız bir oyunun üzerinde kalırken oyunu simge durumuna küçültmeden kumanda odağını alabilir.
- Desteklenen GameInput yollarında oyun içi arayüzde gezinmek artık oyunu da kontrol etmiyor.
- Oyun içi arayüzü kapattığınızda, kumanda oyuna geri verilmeden önce basılı düğmelerin ve çubukların serbest konuma dönmesi kısa bir süre beklenir; böylece basılı tutulan bir giriş oyuna aktarılmaz.
- Oyun içi arayüzü açıp kapatmak artık daha hızlı ve güvenilir; hızlıca tekrar basmak arayüzü açıldıktan hemen sonra kapatmaz. Alt-Tab ve başka bir uygulamaya bilinçli olarak yapılan diğer geçişler dikkate alınır.
- Oyun penceresini yeniden oluşturduğunda, penceresini kaybettiğinde veya odağı kısa süreliğine kaybettiğinde oyun içi arayüz doğru oyunu izlemeye devam eder. Başka bir uygulama öne geçtiğinde sorunsuz şekilde kapanır.
- Oyun içi arayüz, en son seçtiğiniz ekran görüntüsü veya klip ile yeniden açılır. Yeni bir çekimden sonra en yeni öğeden başlar.
- Klipler, önizleme kısa süreliğine kaybolmadan oyun içi arayüzde oynatılmaya başlar.
- Karartılmış arka plan artık menülerden sonra yavaşça belirmek yerine oyun içi arayüz menüleriyle birlikte görünüyor.
- Alt kısımdaki kontrol ipuçları artık oyun içi arayüz karartması ayarınıza uyan küçük bir arka plan üzerinde gösteriliyor; böylece oyunun üzerinde de okunaklı kalıyor.
- Kenar çubuğunda gezinirken şu anda oynadığınız oyun artık doğru şekilde vurgulanıyor.
- Oyun içi arayüz menülerinde, galeride ve video oynatmada kumandayla gezinme iyileştirildi; gezinme düğmeleri çekim kısayollarından ayrı tutuluyor.

## Çekim ve anında tekrar

- Art arda hızlı kayıtlar, mevcut bir klibin üzerine yazmak yerine benzersiz dosya adları alır. Başarısız bir dışa aktarma önceki klipleri asla silmez ve küçük resimler her zaman doğru kliple eşleşir.
- Anında tekrar kaydı, ayarladığınız arabellek süresi dolmadan önce bile o ana kadar kaydedilen görüntüleri kullanır.
- Oyun değiştirdiğinizde veya arabellek yeniden başladığında anında tekrar dışa aktarması ihtiyaç duyduğu görüntüleri korur; GameHQ da kapanmadan önce devam eden dışa aktarmanın bitmesini bekler.
- Ekran görüntüleri ve anında tekrar kayıtları hemen onaylanır, ardından kaydın başarılı mı başarısız mı olduğu açıkça gösterilir. Bildirimler yerinde güncellenir ve aynı anda yalnızca sınırlı sayıda bildirim gösterilir.
- Başarısız olan veya atlanan çekimler, başarı bildirimleri kapalı olsa bile artık nedenini açıklıyor.
- Anında tekrar durumu artık kaydın gerçekten başlayıp başlamadığını ve kullanılabilir görüntü olup olmadığını gösteriyor; başlatılıyor, arabellek boş ve dışa aktarma sürüyor durumları için açık mesajlar sunuluyor.
- Manuel anında tekrar oturumları artık ayar değişikliklerinden ve aynı anda alınan HDR ekran görüntülerinden etkilenmiyor. Başlattığınız ancak hiç kullanmadığınız manuel oturum bir süre sonra kapanır.
- Anında tekrar kaydedilirken küçük resim oluşturulması artık çekimi durdurmuyor; aynı anda alınan ekran görüntüleri de klasörlerini daha güvenilir şekilde oluşturuyor.
- Windows'un sarı çekim kenarlığı için yeni bir seçenek eklendi; izin ve sistem desteği hakkındaki bilgiler de daha anlaşılır hâle getirildi. Windows kenarlığı gizleyemediğinde de kayıt çalışmaya devam eder.

## Kumanda ve giriş

- Birden fazla kumanda veya giriş kaynağı bağlıyken kumanda algılama ve giriş yönlendirme artık daha güvenilir.
- Giriş kaynakları arasında geçiş, kumandaların yeniden bağlanması ve basılı düğmelerin takibi artık kaçırılan basışları, çift basışları ve takılı kalan girişleri önlüyor.
- PS düğmesine basışların algılanmaması ve oyun içi arayüzü kapattıktan hemen sonra yeniden açabilen gecikmeli tekrar basışlar düzeltildi.
- Tetikler, çubuk basışları ve diğer düğmeler artık desteklenen tüm giriş kaynaklarında aynı şekilde davranıyor; böylece atamalar daha öngörülebilir çalışıyor.
- GameInput kullanan kumandalar, Windows isteğe bağlı Guide/Share düğmesi desteğini sağlayamadığında da oyun içi arayüz yalıtımı dâhil normal şekilde çalışmaya devam eder.
- Ayarlar'da düzenlediğiniz kumanda, başka bir kumanda etkin hâle geldiğinde de seçili kalır.

## Eşleme ön ayarları

- Kumanda düzenleri için yeni ön ayar kitaplığı: ön ayarları oluşturabilir, yeniden adlandırabilir, çoğaltabilir, düzenleyebilir ve silebilirsiniz.
- Ön ayarlar kumandalara ve oyunlara atanabilir; bir yedek seçenek ve çalıştırdığınız oyun için otomatik seçim sunulur.
- Mevcut özel atamalarınız ön ayar sistemine otomatik olarak taşınır ve özgün veriler yedek olarak saklanır.
- Ön ayar değiştirilirken basılı düğmeler ve devam eden hareketler dikkate alınır; böylece geçiş yanlışlıkla eylem tetiklemez.
- Arayüz, atanmış ön ayarı düzenlediğiniz ön ayardan ayırır, kaydedilmemiş düzenlemeleri korur ve tek bir kumanda için kopya oluşturmanıza olanak tanır.
- Kullanımdaki bir ön ayarı silmeden önce yerine geçecek bir ön ayar veya yedek seçmeniz istenir; paylaşılan ön ayarlardaki değişiklikler açıkça belirtilir.

## Arayüz, ayarlar ve ses

- GameHQ, en son kullandığınız sayfa, Ayarlar kategorisi ve galeri filtresiyle yeniden açılır. Oyun içi arayüz son kategorisini her oyun için ayrı hatırlar.
- Pencere artık ana ekranınızın solundaki veya üstündeki monitörlerde doğru şekilde geri yükleniyor. Tamamen ekran dışında açılacak bir pencere bağlı bir ekrana geri taşınır.
- Ana pencere ve oyun içi arayüz, %100 ile %200 arasında ayrı ayrı ölçeklendirilebilir ve bu ayar yeniden başlatmalardan sonra da hatırlanır. Küçük pencerelerde yerleşim daha düzgün.
- Çekim sesleri daha yüksek ve daha belirgin; ayrıca kendi ses düzeyi denetimleri ve önizlemeleri var. Arayüz ve çekim ses düzeyi %300'e kadar çıkarılabilir.
- Bildirim, ses, çekim kenarlığı veya manuel oturum ayarlarını değiştirmek artık anında tekrar arabelleğinizi silmiyor.
- Galeri açıldığında kayıtlı filtresi korunur. Artık var olmayan bir oyun için kaydedilmiş bir filtre güvenli şekilde varsayılana döner. Ayarlar kategorilerinin sırası değişse de seçili kategori yerinde kalır.
- Arayüz ölçeği değiştirilirken görülen boş içerik ve katman sırası sorunları düzeltildi; menüler artık ana içeriğin üzerinde gösteriliyor.
- Atama kontrolleri artık basılı tutmanın ne kadar sürdüğünü gösteriyor ve basılı tutma hareketlerinin nasıl çalıştığını açıklıyor.

## Güncellemeler, diller ve tanılama

- Güncelleme notları seçtiğiniz dilde gösterilir, çeviri yoksa İngilizce gösterilir ve bir kez yüklendikten sonra çevrimdışı da okunabilir.
- Güncelleme notları, kumandayla da erişebileceğiniz isteğe bağlı bir GitHub sürüm bağlantısı içerebilir.
- Yeni çekim, ses, kenarlık, basılı tutma, ön ayar ve odak mesajlarının çevirileri desteklenen tüm dillere eklendi.
- Kopyalanan tanılama bilgileri artık etkin atamaları, ön ayar seçimini, giriş kaynağı değişikliklerini ve oyun içi arayüzün odak durumunu içeriyor; cihaz tanımlayıcıları anonimleştirilir.
- Çekim tanılaması her isteği düğmeye basıldığı andan dosyanın kaydedilmesine kadar izler; bu da sorunların kaynağını bulmayı kolaylaştırır. Ses sorunları açık bir uyarıyla bildirilir.

## Bilinen sınırlamalar

- Oyun içi arayüzün kumandayı oyundan yalıtıp yalıtamayacağı, oyuna ve oyunun kumandayı nasıl okuduğuna bağlıdır. GameInput kullanan kablolu bir DualSense kapsamlı şekilde test edilmiştir. XInput, Raw Input, doğrudan HID, Steam Input veya sanal kumandalar için garanti edilmez.
- DSX çalışırken kumanda değiştirme yalnızca kısmen doğrulanmıştır. DSX kurulumları sizin denetiminizde kalır; GameHQ sanal kumanda veya cihaz gizleme sürücülerini yüklemez ya da yönetmez.
- Bazı oyunlar odağı kaybettiğinde duraklar veya tepki verir. Windows ya da başka bir çekim uygulaması sarı kayıt kenarlığını görünür tutabilir.
- 0.7.8 sürüm notlarının yerelleştirilmiş hâli bulunmadığında GameHQ İngilizce sürümü gösterir.
