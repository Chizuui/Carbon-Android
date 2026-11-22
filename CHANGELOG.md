# Changelog — NFS Carbon Android (`carbon-port`)

- Crash pas mau start balapan dibenerin — habis fix crash world map kemarin, load balapan masih crash di fungsi lain (240DE0) karena database mobil career-nya memang rusak (tidak ada mobil yang valid sama sekali); sekarang fungsi itu juga dilewatin kalau database mobilnya rusak, jadi tidak crash lagi.
- Cheat UNLOCK ALL dihapus total — barisnya sudah tidak ada lagi di menu CHEATS, dan kodenya juga dihapus, jadi tidak ada lagi cara buat buka semua mobil/part/balapan sekaligus; mobil yang dibeli tetap muncul seperti biasa karena fix beli mobil kemarin (yang nandain mobil sebagai dimiliki) masih jalan.
- Build diagnostik buat bug map keluar radar — build ini nyatetin tiap gambar yang pakai stencil (mask lingkaran radar) biar ketahuan kenapa jalanan bisa keluar dari lingkaran; bukan fix, tapi buat nyari penyebabnya dari log.
- Bulatan kuning (target ring) di minimap sekarang ikut gerak pas minimap dipindah — sebelumnya ring-nya ketinggalan di posisi map yang lama karena kode yang nempatin ring itu baca posisi map dari tempat yang tidak ikut kegeser; sekarang tiga fungsi minimap itu juga digeser bareng map-nya.
- Mobil tidak putih lagi di garasi / paint shop — di luar balapan, cat mobil pakai env map ketiga yang belum dikenali port, jadi catnya ketimpa langit siang bawaan game dan jadi putih; sekarang env map itu juga dikenali dan mobil dapat refleksi abu-abu gelap kayak env map lainnya.
- Crash di garasi dibenerin — pas game nyari nama mobil yang handle-nya tidak ada di database mobil career, lookup-nya ngasih NULL dan game crash pas baca nama mobil itu; sekarang kalau objek mobilnya NULL langsung dikasih 0 (sama kayak yang game sendiri lakuin di tempat lain), jadi tidak crash lagi.
- Crash pas dari world map mau loading balapan sudah diperbaiki — guard yang benerin mobil career cuma jalan pas flow career (3), padahal game juga manggil fungsi itu pas flow world map/loading (5) dan crash karena handle mobilnya jelek; sekarang repair-nya jalan kapanpun career sudah ke-load, jadi crash yang sama tidak terulang.
- Crash di world map dibenerin — kalau database mobil career tidak punya satu pun mobil yang valid (tidak ada mobil pengganti sama sekali), game sebelumnya tetap maksa lanjut dan crash pas nyari mobilnya; sekarang proses load mobilnya dilewatin aja biar tidak crash, dan log-nya nyatet kenapa.
- Hook SearchForString dihapus total — lookup string game sekarang jalan tanpa hook sama sekali, jadi semua teks tampil apa adanya dari game; efek sampingnya, string yang tidak ada terjemahannya kembali nunjukkin placeholder "UNLOCALIZED" (sebelumnya dibersihkan jadi string kosong).
- "Tap anywhere to begin" dibalikin ke aslinya — layar judul sekarang nunjukkin "Press START" lagi dan tap di layar kosong tidak lagi lanjut; lanjutinnya pakai tombol START (pill START di layar atau Start di gamepad), dan tap di menu cuma milih tombol, bukan konfirmasi.
- Mobil tidak putih lagi pas PANTULAN MOBIL di OFF — pas OFF, muka cube refleksi tidak pernah digambar, jadi port tidak kenal lagi cube-nya dari target yang ada dan mobil jatuh ke langit siang bawaan game; sekarang port mengenali cube-nya dari objek teksturnya sendiri (live cube dan STATICENVMAP), jadi mobil dapat refleksi abu-abu gelap.
- Mobil tidak putih lagi pas PANTULAN MOBIL bukan FULL — kalau cube refleksi belum sempat digambar (habis loading, atau rate di bawah FULL), mobil sekarang dapat refleksi abu-abu gelap, bukan cube langit siang bawaan game; plus cube dipaksa digambar di awal balapan biar langsung normal.
- Vinyl di editor akhirnya bisa digeser — layarnya ternyata baca left stick, bukan sentuhan, jadi drag di vinyl sekarang diteruskan ke stick (D-pad atas/bawah juga gerak vertikal), dan swipe di layar itu tidak terbalik lagi.
- Mobil yang baru dibeli tidak hilang lagi dari car select pas cheat UNLOCK ALL mati — pas beli, game cuma masukin mobil ke database tapi tidak nandain mobil itu sebagai "dimiliki" di tabel unlock, jadi daftar mobil menyembunyikannya; sekarang tiap mobil dibeli langsung ditandai sebagai dimiliki, jadi tetap kelihatan walau cheat mati.
- Bersih-bersih sisa Most Wanted — label game sekarang "Carbon" (judul log tidak lagi dobel "Need for Speed: NFS Carbon"), kredit Java diperbaiki jadi "Port by Chizui" + "Base by Rangga Casper" dengan link Donate/GitHub/YouTube yang benar, caption tombol BLACK/WHITE jadi "CHANGE CAMERA"/"REAR VIEW", dan alamat-alamat MW yang basi di komen sudah dibersihkan; file XBE Most Wanted yang tidak terpakai (`ci/default.xbe.enc`) dihapus.
- EXPORT LOG sekarang selalu mendarat di Download — kalau log sedang di folder fallback (folder aplikasi sendiri), jalurnya diambil dari folder Carbon, bukan lagi nyasar ke dalam Android/data.
- DumpGuest sekarang memakai ukuran image Carbon yang benar (berakhir di 0x4DE7C0, bukan 0x508040).

- SKIP MOVIES dimatikan dulu — setelah dicek lebih dalam, alamat yang ditulis tiap frame ternyata belum terbukti sebagai flag skip-movies (tidak ada kaitannya sama movie player), jadi tulisannya dihapus biar tidak merusak opsi game; tombolnya tetap ada tapi belum ada efeknya sampai terbukti.
- Sun glare balik lagi — game ternyata memakai hasil tes visibilitas buat ngatur terang glare matahari, tapi port selalu jawab 0 sehingga glare mengecil sampai hilang; sekarang dijawab "matahari kelihatan penuh", jadi glare muncul lagi pas matahari di langit.
- Guard mobil career sekarang cuma jalan pas career beneran ke-load (GameFlowState==3, sama seperti game-nya) — log "car_db empty" yang spam pas career belum ke-load hilang; plus fallback guard tidak lagi milih mobil cuma berdasarkan flag yang belum terbukti artinya.
- Lubang crash di pemanggil guard stat mobil ditutup — fungsi pemanggilnya baca ulang pointer mobil sendiri setelah guard, jadi ditambah validasi di pemanggil itu supaya pointer rusak tidak bikin crash lagi.

- Log sesi sebelumnya tidak ketimpa lagi pas buka app — log lama disimpan jadi `carbon-prev.log`, dan EXPORT LOG ikut menyalinnya ke Download, jadi jejak crash tetap bisa dikirim walau app sudah dibuka lagi.

- Mobil di safe house tidak hilang lagi pas pilih car select — guard data mobil sekarang pakai patokan "dimiliki" yang sama seperti game (flag owned di database mobil), jadi mobil yang sudah dibeli tidak ketukar sama slot kosong; plus cheat MAX CASH tidak lagi nulis ke alamat yang salah kalau career belum ke-load.

- Auto-update dari GitHub release — app sekarang ngecek update sendiri tiap dibuka (ada juga tombol CHECK FOR UPDATES di settings); kalau ada versi baru, muncul changelog-nya sama tombol Download, jadi nggak perlu nunggu link manual lagi.
- Link Donate, GitHub, dan YouTube diupdate — Donate ke Sociabuzz Chizui, GitHub ke repo Carbon-Android, YouTube ke channel Sashachan.
- Overlay FPS sekarang cuma gaya GOverlay — panel gelap, bar load, sama grafik frame time; pilihan gaya lain dihapus. Ada metrik baru LATENCY (rata-rata frame time, misal "21.7 ms", warnanya hijau/kuning/merah); layout vertikal/horizontal tetap bisa dipilih.
- Overlay FPS dirombak — gaya AFTERBURNER / GOVERLAY / CARBON, layout vertikal atau horizontal, angka FPS dan suhu berubah warna (hijau/kuning/merah), GOverlay ada bar load sama grafik frame time; semua bisa diatur di Tampilan > OVERLAY PERFORMA.
- UI settings dirombak total — panel sidebar (Tampilan / Controller / Touch / Umum / Cheat / Kredit), halaman Controller ada tes input gamepad langsung dan rebind dikelompokkan, ada slider deadzone stick; editor layout touch tetap fullscreen, cuma tampilannya disesuaikan tema baru.
- Crash abis menang bos diperbaiki (lanjutan) — guard ditambah ke satu lagi fungsi baca stat mobil yang ikut crash karena data mobil di career kosong; plus log diagnosa buat nyari letak data mobil yang bener di career.
- Crash abis menang bos diperbaiki — game tidak lagi ketutup sendiri pas balik ke garasi, penyebabnya pointer mobil yang rusak karena data mobil di career kosong.
- Tap di menu tidak auto-confirm lagi — tap buat milih mobil bos atau huruf pas ketik nama cuma milih/highlight aja, confirm-nya pencet tombol A sendiri.
- Rebind trigger beneran tidak dobel lagi — bind lama bentuk tombol juga ikut dibersihkan, jadi R2 cuma gas, kamera tidak ikut ganti tiap tap gas.
- Rebind trigger tidak dobel lagi — pas ganti Right Trigger dari R1 ke R2, R2 cuma jalanin Right Trigger, tidak ikut kepencet fungsi lamanya.
- Analog tidak tembus lagi pas buka settings — stick menu depan disembunyikan selama panel settings terbuka.
- JOIN DISCORD ditambah di GENERAL, di sebelah EXPORT LOG — tap untuk buka invite Discord buat lapor bug.
- EXPORT LOG ditambah di GENERAL — sekali tap, log sesi ini disalin ke Download sebagai `carbon-log-<tanggal>-<jam>.log`, siap dikirim ke developer.
- YELLOW TINT dihapus dari menu Carbon — setting itu mati, tidak ada efek di Carbon.
- RENDER RESOLUTION menampilkan ukuran 3D yang benar (sebelumnya lebar dilebih-lebihkan 4/3x).
- BLOOM OFF sekarang mematikan seluruh rantai bloom, bukan cuma add-back — preset low-end lebih hemat GPU.
- Composite fullscreen di flare pass di-skip — sumber kabut putih saat LIGHT FLARES on.
- Preset MID dan HIGH ditambah di samping LOW-END (DISPLAY > PERFORMANCE).
- Preset LOW-END: sekali tap set render 50%, bloom off, motion blur off, reflections QUARTER, filter off.
- Present shader lebih murah: gamma pakai LUT, tidak ada pow() per piksel lagi.
- Reflection cube dibatasi ke skala efek — tidak render 6 view di resolusi penuh lagi.
- Jejak kotak hitam di HUD (minimap, EA Trax, race info, speedometer) dihilangkan — latar panel ikut terang.
- Guard giant-streak dan spotlight headlight dihapus — guard malah merusak pause blur dan fade.
- Fan dan trail flare di-skip — gambar statik di Vulkan hilang, layar bersih.
- Boss Bonus Cards tidak flicker disco lagi — host mengikuti buffer header title di Swap.
- Kotak hitam di HUD diperbaiki — mask kecerahan digate ke luma piksel.
- LIGHT FLARES digate ke setting (default OFF) — tidak ada efek kalau mati.
- Menu analog knob dan D-Pad diperbaiki, caption Carbon diperbarui.
