const books = [
  { id: 'night-watch', title: 'Ночной дозор', author: 'Терри Пратчетт', category: 'Фантастика', year: 1989, cover: 'cover-night', description: 'Город просыпается, когда остальные засыпают. История о долге, юморе и маленьких решениях, которые меняют большой мир.' },
  { id: 'sapiens', title: 'Sapiens. Краткая история человечества', author: 'Юваль Ной Харари', category: 'Нон-фикшн', year: 2011, cover: 'cover-amber', description: 'Путешествие от первых людей до технологической революции и вопроса о том, кем мы станем дальше.' },
  { id: 'master-margarita', title: 'Мастер и Маргарита', author: 'Михаил Булгаков', category: 'Классика', year: 1967, cover: 'cover-plum', description: 'Москва, весна, загадочный незнакомец и история, в которой реальность легко уступает место чуду.' },
  { id: 'east-express', title: 'Убийство в Восточном экспрессе', author: 'Агата Кристи', category: 'Детектив', year: 1934, cover: 'cover-red', description: 'Снежный занос остановил поезд. Один из пассажиров мёртв, а Эркюль Пуаро начинает задавать вопросы.' },
  { id: 'moby-dick', title: 'Моби Дик, или Белый кит', author: 'Герман Мелвилл', category: 'Классика', year: 1851, cover: 'cover-sand', description: 'Морское путешествие, одержимость и команда китобойного судна, отправившаяся навстречу легенде.' },
  { id: 'solaris', title: 'Солярис', author: 'Станислав Лем', category: 'Фантастика', year: 1961, cover: 'cover-green', description: 'На далёкой станции учёные пытаются понять океан, который, возможно, изучает их в ответ.' },
  { id: 'road', title: 'Дорога', author: 'Кормак Маккарти', category: 'Фантастика', year: 2006, cover: 'cover-night', description: 'Отец и сын идут на юг через выжженный мир, сохраняя огонь человечности.' },
  { id: 'letters', title: 'Письма к молодому поэту', author: 'Райнер Мария Рильке', category: 'Нон-фикшн', year: 1929, cover: 'cover-amber', description: 'Разговор о творчестве, одиночестве и внимании к собственной внутренней жизни.' },
  { id: 'idiot', title: 'Идиот', author: 'Фёдор Достоевский', category: 'Классика', year: 1869, cover: 'cover-plum', description: 'Князь Мышкин возвращается в Россию и оказывается в центре сложных отношений и страстей.' },
  { id: 'hound', title: 'Собака Баскервилей', author: 'Артур Конан Дойл', category: 'Детектив', year: 1902, cover: 'cover-red', description: 'Легенда о чёрной собаке приводит Холмса и Ватсона на туманные болотистые земли Девона.' },
  { id: 'dune', title: 'Дюна', author: 'Фрэнк Герберт', category: 'Фантастика', year: 1965, cover: 'cover-sand', description: 'Пустынная планета, борьба домов и судьба юноши, которому предстоит стать частью большой истории.' },
  { id: 'twelve', title: 'Двенадцать стульев', author: 'Илья Ильф и Евгений Петров', category: 'Классика', year: 1928, cover: 'cover-green', description: 'Остап Бендер и Киса Воробьянинов отправляются на поиски сокровищ, спрятанных в одном из стульев.' },
];

const state = { query: '', category: 'all', page: 1, pageSize: 6, paired: false, email: 'reader@example.com', autoDownload: false, delivery: {} };
const $ = (selector) => document.querySelector(selector);
const escapeHtml = (value) => String(value).replace(/[&<>\"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;', "'": '&#039;' }[char]));

function filteredBooks() {
  const query = state.query.trim().toLowerCase();
  return books.filter((book) => {
    const matchesCategory = state.category === 'all' || book.category === state.category;
    const matchesQuery = !query || (book.title + ' ' + book.author).toLowerCase().includes(query);
    return matchesCategory && matchesQuery;
  });
}

function coverMarkup(book, detail) {
  return '<div class="cover ' + book.cover + (detail ? ' detail-cover' : '') + '"><span class="cover-kicker">bookrelay / demo</span><span class="cover-title">' + escapeHtml(book.title) + '</span><span class="cover-author">' + escapeHtml(book.author) + '</span></div>';
}

function render() {
  const matches = filteredBooks();
  const pages = Math.max(1, Math.ceil(matches.length / state.pageSize));
  state.page = Math.min(state.page, pages);
  const visible = matches.slice((state.page - 1) * state.pageSize, state.page * state.pageSize);
  $('#results-heading').textContent = state.query ? 'Результаты для «' + state.query + '»' : (state.category === 'all' ? 'Рекомендуем почитать' : state.category);
  $('#result-count').textContent = matches.length + ' ' + (matches.length === 1 ? 'книга' : 'книг');
  $('#page-label').textContent = 'Страница ' + state.page + ' из ' + pages;
  $('#previous-page').disabled = state.page === 1;
  $('#next-page').disabled = state.page >= pages;
  $('#empty-state').classList.toggle('hidden', visible.length > 0);
  $('#book-grid').classList.toggle('hidden', visible.length === 0);
  $('#book-grid').innerHTML = visible.map((book) => '<article class="book-card" data-book-id="' + book.id + '">' + coverMarkup(book, false) + '<div class="book-content"><h3>' + escapeHtml(book.title) + '</h3><p class="book-author">' + escapeHtml(book.author) + ' · ' + book.year + '</p><p class="book-description">' + escapeHtml(book.description) + '</p><div class="book-footer"><span class="book-category">' + escapeHtml(book.category) + '</span><button class="details-button" data-action="details" data-book-id="' + book.id + '" type="button">Подробнее</button></div></div></article>').join('');
  $('#book-grid').querySelectorAll('.book-card').forEach((card) => card.addEventListener('click', (event) => {
    if (event.target.closest('button')) return;
    openDetails(card.dataset.bookId);
  }));
  $('#book-grid').querySelectorAll('[data-action=details]').forEach((button) => button.addEventListener('click', () => openDetails(button.dataset.bookId)));
}

function setStatus(text) { $('#status-text').textContent = text; }
function openModal(content) {
  const root = $('#modal-root');
  root.innerHTML = '<div class="modal" role="dialog" aria-modal="true">' + content + '</div>';
  root.classList.remove('hidden');
  root.addEventListener('click', (event) => { if (event.target === root) closeModal(); }, { once: true });
  root.querySelector('.close-button')?.addEventListener('click', closeModal);
}
function closeModal() { $('#modal-root').classList.add('hidden'); $('#modal-root').innerHTML = ''; }

function openDetails(bookId) {
  const book = books.find((item) => item.id === bookId);
  if (!book) return;
  const currentDelivery = state.delivery[book.id];
  const deliveryMarkup = currentDelivery ? '<div class="delivery-state">' + escapeHtml(currentDelivery) + '</div>' : '';
  const sendLabel = currentDelivery === 'Отправлено relay · ожидайте Amazon' ? 'Отправлено' : 'Скачать на Kindle';
  openModal('<div class="modal-header"><div><p class="eyebrow">КАРТОЧКА КНИГИ</p><h2>' + escapeHtml(book.title) + '</h2><p class="modal-subtitle">' + escapeHtml(book.author) + '</p></div><button class="close-button" type="button" aria-label="Закрыть">×</button></div><div class="detail-layout">' + coverMarkup(book, true) + '<div><p class="detail-description">' + escapeHtml(book.description) + '</p><p class="detail-meta">Автор: ' + escapeHtml(book.author) + '<br>Год: ' + book.year + '<br>Категория: ' + escapeHtml(book.category) + '<br>Формат: EPUB</p></div></div>' + deliveryMarkup + '<div class="modal-actions"><button class="outline-button close-action" type="button">Закрыть</button><button class="ink-button send-action" type="button">' + sendLabel + '</button></div>');
  $('.close-action')?.addEventListener('click', closeModal);
  $('.send-action')?.addEventListener('click', () => sendBook(book));
}

function sendBook(book) {
  if (!state.paired) {
    closeModal();
    setStatus('Сначала выполните pairing в настройках');
    openPairing();
    return;
  }
  state.delivery[book.id] = 'Задание создано · relay готовит EPUB';
  setStatus('Отправка «' + book.title + '»…');
  openDetails(book.id);
  window.setTimeout(() => { state.delivery[book.id] = 'EPUB подготовлен · отправка на Amazon'; openDetails(book.id); setStatus('EPUB отправлен relay · ожидайте доставку Amazon'); }, 850);
  window.setTimeout(() => { state.delivery[book.id] = 'Отправлено relay · ожидайте Amazon'; openDetails(book.id); }, 1900);
}

function openPairing() {
  const code = 'K7M4-29QX';
  openModal('<div class="modal-header"><div><p class="eyebrow">ПОДКЛЮЧЕНИЕ УСТРОЙСТВА</p><h2>Pairing Kindle</h2><p class="modal-subtitle">Одноразовый код действует 10 минут</p></div><button class="close-button" type="button" aria-label="Закрыть">×</button></div><div class="pairing-code"><small>КОД PAIRING</small><strong>' + code + '</strong><small>Откройте страницу relay на компьютере</small></div><p class="helper">Введите этот код на странице привязки вместе с адресом Kindle Email. После подтверждения токен сохранится в пользовательской области приложения.</p><div class="modal-actions"><button class="outline-button close-action" type="button">Закрыть</button><button class="ink-button paired-action" type="button">Симулировать привязку</button></div>');
  $('.close-action')?.addEventListener('click', closeModal);
  $('.paired-action')?.addEventListener('click', () => { state.paired = true; closeModal(); $('#connection-status').textContent = 'Kindle: ' + state.email; setStatus('Kindle привязан · можно отправлять книги'); });
}

function openSettings() {
  openModal('<div class="modal-header"><div><p class="eyebrow">ЛОКАЛЬНАЯ КОНФИГУРАЦИЯ</p><h2>Настройки</h2><p class="modal-subtitle">Настройки сохраняются на Kindle</p></div><button class="close-button" type="button" aria-label="Закрыть">×</button></div><label class="field">RELAY URL<input id="relay-url" value="https://relay.example.com" spellcheck="false"></label><label class="field">KINDLE EMAIL<input id="kindle-email" value="' + escapeHtml(state.email) + '" type="email"></label><div class="toggle-row"><span>Автоматически отправлять новые книги</span><input id="auto-download" class="toggle" type="checkbox" ' + (state.autoDownload ? 'checked' : '') + '></div><p class="helper">В preview relay не вызывается: кнопка имитирует только пользовательский сценарий.</p><div class="modal-actions"><button class="outline-button close-action" type="button">Отмена</button><button class="ink-button save-settings" type="button">Сохранить</button></div>');
  $('.close-action')?.addEventListener('click', closeModal);
  $('.save-settings')?.addEventListener('click', () => { state.email = $('#kindle-email').value || 'reader@example.com'; state.autoDownload = $('#auto-download').checked; closeModal(); setStatus('Настройки сохранены'); if (state.paired) $('#connection-status').textContent = 'Kindle: ' + state.email; });
}

$('#search-form').addEventListener('submit', (event) => { event.preventDefault(); state.query = $('#search-input').value; state.page = 1; render(); setStatus(state.query ? 'Поиск завершён · ' + filteredBooks().length + ' результатов' : 'Показаны рекомендации'); });
$('#search-input').addEventListener('search', () => { state.query = $('#search-input').value; state.page = 1; render(); });
document.querySelectorAll('.category').forEach((button) => button.addEventListener('click', () => { state.category = button.dataset.category; state.page = 1; document.querySelectorAll('.category').forEach((item) => item.classList.toggle('active', item === button)); render(); setStatus('Категория: ' + button.textContent); }));
$('#previous-page').addEventListener('click', () => { if (state.page > 1) { state.page -= 1; render(); setStatus('Открыта страница ' + state.page); } });
$('#next-page').addEventListener('click', () => { const pages = Math.max(1, Math.ceil(filteredBooks().length / state.pageSize)); if (state.page < pages) { state.page += 1; render(); setStatus('Открыта страница ' + state.page); } });
$('#pair-button').addEventListener('click', openPairing);
$('#settings-button').addEventListener('click', openSettings);
document.addEventListener('keydown', (event) => { if (event.key === 'Escape') closeModal(); });
render();
