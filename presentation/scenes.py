# SPDX-License-Identifier: GPL-3.0-or-later
"""The presentation video, scene by scene: what the app shows (a script for the host build, against the mock
engine's made-up catalogue), what the English voice says, and the subtitles in the app's nine languages.

kind "card": a title card drawn by make_video.py. kind "app": the app recorded at 1080p; "skip" seconds of the
recording are cut first (the start screen, the moves that lead to the shown screen). "say" is the text given to
the speech synthesis (spelled for it); "text" is what the subtitles show."""

LANGS = ["en", "fr", "es", "it", "ru", "pt", "zh", "th", "vi"]
ISO639_2 = {"en": "eng", "fr": "fra", "es": "spa", "it": "ita", "ru": "rus", "pt": "por", "zh": "zho", "th": "tha", "vi": "vie"}

SCENES = [
    {
        "id": "intro", "kind": "card", "min": 6.0,
        "card": ["exFAT2AMPR", "PS5 game dumps to AMPR LZ4, right on the console", "Made by V-10  ·  LZ4 format: DLPSGAME"],
        "say": "Meet ex FAT to A M P R. The app that converts your P S 5 game dumps to the A M P R, L Z 4 format, right on the console.",
        "text": {
            "en": "Meet exFAT2AMPR: the app that converts your PS5 game dumps to the AMPR LZ4 format, right on the console.",
            "fr": "Voici exFAT2AMPR : l'app qui convertit vos dumps de jeux PS5 au format AMPR LZ4, directement sur la console.",
            "es": "Te presentamos exFAT2AMPR: la app que convierte tus dumps de juegos de PS5 al formato AMPR LZ4, directamente en la consola.",
            "it": "Ecco exFAT2AMPR: l'app che converte i dump dei tuoi giochi PS5 nel formato AMPR LZ4, direttamente sulla console.",
            "ru": "Знакомьтесь: exFAT2AMPR — приложение, которое конвертирует дампы игр PS5 в формат AMPR LZ4 прямо на консоли.",
            "pt": "Conheça o exFAT2AMPR: o app que converte os dumps dos seus jogos de PS5 para o formato AMPR LZ4, direto no console.",
            "zh": "认识一下 exFAT2AMPR：直接在主机上把 PS5 游戏转储转换为 AMPR LZ4 格式的应用。",
            "th": "ขอแนะนำ exFAT2AMPR แอปที่แปลงไฟล์ดัมพ์เกม PS5 เป็นรูปแบบ AMPR LZ4 ได้โดยตรงบนคอนโซล",
            "vi": "Giới thiệu exFAT2AMPR: ứng dụng chuyển đổi bản dump trò chơi PS5 sang định dạng AMPR LZ4 ngay trên máy.",
        },
    },
    {
        "id": "library", "kind": "app", "skip": 3.4, "min": 8.0,
        "script": "wait:3400 wait:1200 down wait:1700 down wait:1700 down wait:1700 down wait:1600",
        "say": "It finds every game and app on your console: on the internal storage, on U S B drives, or on the M dot 2 S S D. "
               "And it shows where each one is kept, with its cover.",
        "text": {
            "en": "It finds every game and app on your console, on the internal storage, on USB drives or on the M.2 SSD, and shows where each one is kept, with its cover.",
            "fr": "Elle trouve chaque jeu et chaque application de la console, sur le stockage interne, les disques USB ou le SSD M.2, et indique où chacun se trouve, avec sa jaquette.",
            "es": "Encuentra todos los juegos y apps de tu consola, en el almacenamiento interno, en unidades USB o en el SSD M.2, y muestra dónde está cada uno, con su portada.",
            "it": "Trova ogni gioco e app della console, nella memoria interna, sulle unità USB o sull'SSD M.2, e mostra dove si trova ciascuno, con la sua copertina.",
            "ru": "Оно находит все игры и приложения на консоли — во внутренней памяти, на USB-накопителях или на SSD M.2 — и показывает, где хранится каждое, вместе с обложкой.",
            "pt": "Ele encontra todos os jogos e apps do console, no armazenamento interno, em discos USB ou no SSD M.2, e mostra onde cada um está, com a capa.",
            "zh": "它会找到主机上的所有游戏和应用，无论在内置存储、USB 驱动器还是 M.2 固态硬盘上，并显示每个的位置和封面。",
            "th": "แอปจะค้นหาทุกเกมและแอปในคอนโซล ทั้งในที่เก็บข้อมูลภายใน ไดรฟ์ USB หรือ SSD M.2 พร้อมบอกตำแหน่งและแสดงภาพปกของแต่ละเกม",
            "vi": "Ứng dụng tìm mọi trò chơi và ứng dụng trên máy, trong bộ nhớ trong, ổ USB hay SSD M.2, và cho biết vị trí của từng cái, kèm ảnh bìa.",
        },
    },
    {
        "id": "convert", "kind": "app", "skip": 3.4, "min": 9.0,
        "script": "wait:3400 wait:500 square wait:500 down wait:300 down wait:300 down wait:400 square wait:900 cross wait:1500 "
                  "down wait:600 cross wait:1700 down wait:700 cross wait:1300 up wait:600 cross wait:3400",
        "say": "Pick one game or several, choose where to save them, and the app checks everything first: "
               "free space, A M P R compatibility, and copies that are already converted.",
        "text": {
            "en": "Pick one game or several, choose where to save them, and the app checks everything first: free space, AMPR compatibility and copies already converted.",
            "fr": "Choisissez un ou plusieurs jeux et leur destination : l'app vérifie tout avant de commencer, l'espace libre, la compatibilité AMPR et les copies déjà converties.",
            "es": "Elige uno o varios juegos y dónde guardarlos, y la app lo comprueba todo antes: espacio libre, compatibilidad AMPR y copias ya convertidas.",
            "it": "Scegli uno o più giochi e dove salvarli: l'app controlla tutto prima di iniziare, spazio libero, compatibilità AMPR e copie già convertite.",
            "ru": "Выберите одну или несколько игр и место сохранения — приложение заранее всё проверит: свободное место, совместимость с AMPR и уже сконвертированные копии.",
            "pt": "Escolha um ou vários jogos e onde salvá-los, e o app verifica tudo antes: espaço livre, compatibilidade AMPR e cópias já convertidas.",
            "zh": "选择一个或多个游戏以及保存位置，应用会先检查一切：可用空间、AMPR 兼容性以及已转换的副本。",
            "th": "เลือกเกมเดียวหรือหลายเกม และเลือกที่บันทึก แอปจะตรวจสอบทุกอย่างก่อน ทั้งพื้นที่ว่าง ความเข้ากันได้กับ AMPR และสำเนาที่แปลงแล้ว",
            "vi": "Chọn một hoặc nhiều trò chơi và nơi lưu, ứng dụng sẽ kiểm tra mọi thứ trước: dung lượng trống, khả năng tương thích AMPR và các bản đã chuyển đổi.",
        },
    },
    {
        "id": "progress", "kind": "app", "skip": 3.4, "min": 8.0,
        "mock": {"MOCK_RUNNING": "1", "MOCK_START_PCT": "58", "MOCK_RATE": "6", "MOCK_FINISH_AFTER": "9.5", "MOCK_GAME": "PPSA90101"},
        "script": "wait:3400 wait:9500",
        "say": "The conversion runs in the background, even while you play. Every block is verified, and your original files are kept.",
        "text": {
            "en": "The conversion runs in the background, even while you play. Every block is verified, and your original files are kept.",
            "fr": "La conversion tourne en arrière-plan, même pendant que vous jouez. Chaque bloc est vérifié et vos fichiers d'origine sont conservés.",
            "es": "La conversión se ejecuta en segundo plano, incluso mientras juegas. Se verifica cada bloque y tus archivos originales se conservan.",
            "it": "La conversione prosegue in background, anche mentre giochi. Ogni blocco viene verificato e i file originali vengono conservati.",
            "ru": "Конвертация идёт в фоне, даже во время игры. Каждый блок проверяется, а исходные файлы сохраняются.",
            "pt": "A conversão roda em segundo plano, mesmo enquanto você joga. Cada bloco é verificado e seus arquivos originais são mantidos.",
            "zh": "转换在后台进行，即使你在玩游戏也不受影响。每个数据块都会校验，原始文件会被保留。",
            "th": "การแปลงทำงานอยู่เบื้องหลัง แม้ขณะที่คุณเล่นเกม ทุกบล็อกจะได้รับการตรวจสอบ และไฟล์ต้นฉบับจะถูกเก็บไว้",
            "vi": "Quá trình chuyển đổi chạy nền, kể cả khi bạn đang chơi. Mọi khối dữ liệu đều được kiểm tra và tệp gốc vẫn được giữ lại.",
        },
    },
    {
        "id": "errors", "kind": "app", "skip": 3.4, "min": 6.0,
        "mock": {"MOCK_START_ERROR": "verify_failed"},
        "script": "wait:3400 wait:200 r1 wait:5600",
        "say": "If something goes wrong, the app tells you why, in your language, and what to do next.",
        "text": {
            "en": "If something goes wrong, the app tells you why, in your language, and what to do next.",
            "fr": "Si quelque chose bloque, l'app vous explique pourquoi, dans votre langue, et quoi faire ensuite.",
            "es": "Si algo falla, la app te explica por qué, en tu idioma, y qué hacer a continuación.",
            "it": "Se qualcosa va storto, l'app ti spiega perché, nella tua lingua, e cosa fare.",
            "ru": "Если что-то пошло не так, приложение объяснит причину на вашем языке и подскажет, что делать.",
            "pt": "Se algo der errado, o app explica o motivo, no seu idioma, e o que fazer em seguida.",
            "zh": "如果出现问题，应用会用你的语言说明原因以及下一步该怎么做。",
            "th": "หากเกิดปัญหา แอปจะบอกสาเหตุเป็นภาษาของคุณ พร้อมวิธีแก้ไข",
            "vi": "Nếu có sự cố, ứng dụng sẽ giải thích nguyên nhân bằng ngôn ngữ của bạn và cách xử lý.",
        },
    },
    {
        "id": "languages", "kind": "app", "skip": 3.4, "min": 9.5,
        "script": "wait:3400 r1 r1 r1 wait:900 right wait:950 right wait:950 right wait:950 right wait:950 right wait:950 "
                  "right wait:950 right wait:950 right wait:950 right wait:1200",
        "say": "Nine languages are built in: English, French, Spanish, Italian, Russian, Portuguese, Chinese, Thai, and Vietnamese.",
        "text": {
            "en": "Nine languages are built in: English, French, Spanish, Italian, Russian, Portuguese, Chinese, Thai and Vietnamese.",
            "fr": "Neuf langues sont intégrées : anglais, français, espagnol, italien, russe, portugais, chinois, thaï et vietnamien.",
            "es": "Incluye nueve idiomas: inglés, francés, español, italiano, ruso, portugués, chino, tailandés y vietnamita.",
            "it": "Nove lingue integrate: inglese, francese, spagnolo, italiano, russo, portoghese, cinese, thailandese e vietnamita.",
            "ru": "Встроено девять языков: английский, французский, испанский, итальянский, русский, португальский, китайский, тайский и вьетнамский.",
            "pt": "São nove idiomas integrados: inglês, francês, espanhol, italiano, russo, português, chinês, tailandês e vietnamita.",
            "zh": "内置九种语言：英语、法语、西班牙语、意大利语、俄语、葡萄牙语、中文、泰语和越南语。",
            "th": "มีภาษาในตัวเก้าภาษา ได้แก่ อังกฤษ ฝรั่งเศส สเปน อิตาลี รัสเซีย โปรตุเกส จีน ไทย และเวียดนาม",
            "vi": "Tích hợp chín ngôn ngữ: tiếng Anh, Pháp, Tây Ban Nha, Ý, Nga, Bồ Đào Nha, Trung, Thái và Việt.",
        },
    },
    {
        "id": "pairing", "kind": "app", "skip": 3.4, "min": 7.0,
        "script": "wait:3400 r1 r1 r1 wait:800 down down down down wait:300 cross wait:5500",
        "say": "Pair your phone or P C with a six digit code, or a Q R code, and follow your conversions from the web page.",
        "text": {
            "en": "Pair your phone or PC with a six-digit code or a QR code, and follow your conversions from the web page.",
            "fr": "Associez votre téléphone ou votre PC avec un code à six chiffres ou un QR code, et suivez vos conversions depuis la page web.",
            "es": "Vincula tu teléfono o PC con un código de seis cifras o un código QR y sigue tus conversiones desde la página web.",
            "it": "Associa telefono o PC con un codice a sei cifre o un codice QR e segui le conversioni dalla pagina web.",
            "ru": "Привяжите телефон или ПК шестизначным кодом или QR-кодом и следите за конвертацией на веб-странице.",
            "pt": "Pareie seu celular ou PC com um código de seis dígitos ou um QR code e acompanhe as conversões pela página web.",
            "zh": "用六位数代码或二维码配对手机或电脑，即可在网页上查看转换进度。",
            "th": "จับคู่โทรศัพท์หรือ PC ด้วยรหัสหกหลักหรือคิวอาร์โค้ด แล้วติดตามการแปลงจากหน้าเว็บ",
            "vi": "Ghép nối điện thoại hoặc PC bằng mã sáu chữ số hoặc mã QR, rồi theo dõi quá trình chuyển đổi trên trang web.",
        },
    },
    {
        "id": "duplicates", "kind": "app", "skip": 3.4, "min": 7.5,
        "script": "wait:3400 down down down down down wait:1000 options wait:1100 cross wait:1700 cross wait:3600",
        "say": "Duplicate copies are flagged, and the one you no longer need can be deleted safely.",
        "text": {
            "en": "Duplicate copies are flagged, and the one you no longer need can be deleted safely.",
            "fr": "Les copies en double sont signalées, et celle dont vous n'avez plus besoin se supprime en toute sécurité.",
            "es": "Las copias duplicadas se señalan, y la que ya no necesitas se puede borrar con seguridad.",
            "it": "Le copie doppie vengono segnalate e quella che non ti serve più si elimina in sicurezza.",
            "ru": "Дубликаты отмечаются, а ненужную копию можно безопасно удалить.",
            "pt": "Cópias duplicadas são sinalizadas, e a que você não precisa mais pode ser excluída com segurança.",
            "zh": "重复的副本会被标出，不再需要的那份可以安全删除。",
            "th": "สำเนาที่ซ้ำกันจะถูกทำเครื่องหมาย และลบสำเนาที่ไม่ต้องการได้อย่างปลอดภัย",
            "vi": "Các bản trùng được đánh dấu, và bản bạn không cần nữa có thể xóa an toàn.",
        },
    },
    {
        "id": "outro", "kind": "card", "min": 7.0,
        "card": ["exFAT2AMPR", "Free software  ·  GPL-3.0  ·  version {version}",
                 "Made by V-10  ·  LZ4 format: DLPSGAME  ·  AMPR emulator: drakmor"],
        "say": "ex FAT to A M P R is free software. Made by V 10, with the L Z 4 format by D L P S GAME.",
        "text": {
            "en": "exFAT2AMPR is free software. Made by V-10, with the LZ4 format by DLPSGAME.",
            "fr": "exFAT2AMPR est un logiciel libre. Créé par V-10, avec le format LZ4 de DLPSGAME.",
            "es": "exFAT2AMPR es software libre. Creado por V-10, con el formato LZ4 de DLPSGAME.",
            "it": "exFAT2AMPR è software libero. Creato da V-10, con il formato LZ4 di DLPSGAME.",
            "ru": "exFAT2AMPR — свободное программное обеспечение. Автор — V-10, формат LZ4 — DLPSGAME.",
            "pt": "O exFAT2AMPR é software livre. Criado por V-10, com o formato LZ4 da DLPSGAME.",
            "zh": "exFAT2AMPR 是自由软件。由 V-10 制作，LZ4 格式来自 DLPSGAME。",
            "th": "exFAT2AMPR เป็นซอฟต์แวร์เสรี สร้างโดย V-10 พร้อมรูปแบบ LZ4 จาก DLPSGAME",
            "vi": "exFAT2AMPR là phần mềm tự do. Tác giả V-10, định dạng LZ4 của DLPSGAME.",
        },
    },
]
