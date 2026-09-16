// md2docx.js - converte PROCEDURA.md in un .docx leggibile (titoli, tabelle, codice).
const fs = require('fs');
const d = require('docx');
const {
  Document, Packer, Paragraph, TextRun, HeadingLevel, AlignmentType, Table, TableRow, TableCell,
  WidthType, ShadingType, BorderStyle, TableOfContents, PageBreak, LevelFormat, convertInchesToTwip,
  ExternalHyperlink,
} = d;

const SRC = process.argv[2] || 'PROCEDURA.md';
const OUT = process.argv[3] || 'PROCEDURA_SHASHCHESS42_SF19.docx';
const lines = fs.readFileSync(SRC, 'utf8').split('\n');

const MONO = 'Consolas';
const PAGE_W = 12240 - 2 * 1440;   // Letter meno i margini, in DXA

// --- inline: **grassetto**, `codice`, *corsivo*, annidati fra loro ---
function runs(text, base = {}) {
  const out = [];
  const re = /(\*\*[\s\S]+?\*\*|`[^`]+`|\*[^*`]+\*|\[[^\]]+\]\((?:https?:)?[^)\s]+\))/g;
  let last = 0, m;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) out.push(new TextRun({ text: text.slice(last, m.index), ...base }));
    const t = m[0];
    if (t.startsWith('**')) out.push(...runs(t.slice(2, -2), { ...base, bold: true }));
    else if (t.startsWith('`')) out.push(new TextRun({ text: t.slice(1, -1), font: MONO, size: 19, ...base }));
    else if (t.startsWith('[')) {
      const k = t.indexOf('](');
      out.push(new ExternalHyperlink({
        link: t.slice(k + 2, -1),
        children: runs(t.slice(1, k), { ...base, style: 'Hyperlink' }),
      }));
    } else out.push(...runs(t.slice(1, -1), { ...base, italics: true }));
    last = m.index + t.length;
  }
  if (last < text.length) out.push(new TextRun({ text: text.slice(last), ...base }));
  return out.length ? out : [new TextRun({ text: '', ...base })];
}

function codeParagraph(text, first, lastLine) {
  return new Paragraph({
    children: [new TextRun({ text: text || ' ', font: MONO, size: 18 })],
    spacing: { before: first ? 120 : 0, after: lastLine ? 120 : 0, line: 240 },
    shading: { type: ShadingType.CLEAR, fill: 'F2F2F2' },
    indent: { left: convertInchesToTwip(0.15) },
    keepLines: true,
  });
}

function splitRow(line) {
  return line.trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map(c => c.trim());
}

function makeTable(rows) {
  const hasHeader = rows[0].some(c => c.length > 0);
  if (!hasHeader) rows = rows.slice(1);          // tabella chiave-valore senza intestazione
  const n = Math.max(...rows.map(r => r.length));
  const widths = [];
  // prima colonna piu' stretta se breve, resto distribuito
  const firstShort = rows.every(r => (r[0] || '').length < 14);
  const w0 = firstShort ? Math.round(PAGE_W * 0.16) : Math.round(PAGE_W / n);
  widths.push(w0);
  for (let i = 1; i < n; i++) widths.push(Math.round((PAGE_W - w0) / (n - 1)));
  widths[n - 1] = PAGE_W - widths.slice(0, n - 1).reduce((a, b) => a + b, 0);

  return new Table({
    columnWidths: widths,
    width: { size: PAGE_W, type: WidthType.DXA },
    rows: rows.map((cells, ri) => new TableRow({
      tableHeader: hasHeader && ri === 0,
      children: Array.from({ length: n }, (_, ci) => new TableCell({
        width: { size: widths[ci], type: WidthType.DXA },
        shading: hasHeader && ri === 0 ? { type: ShadingType.CLEAR, fill: 'E7ECF3' } : undefined,
        margins: { top: 60, bottom: 60, left: 100, right: 100 },
        children: [new Paragraph({
          children: runs(cells[ci] || '', hasHeader && ri === 0 ? { bold: true } : {}),
          spacing: { before: 0, after: 0 },
        })],
      })),
    })),
  });
}

const children = [];
let i = 0;
while (i < lines.length) {
  const line = lines[i];

  if (line.startsWith('```')) {                       // blocco di codice
    const buf = [];
    i++;
    while (i < lines.length && !lines[i].startsWith('```')) buf.push(lines[i++]);
    i++;
    buf.forEach((l, k) => children.push(codeParagraph(l, k === 0, k === buf.length - 1)));
    continue;
  }

  if (/^\|/.test(line) && /^\|?\s*:?-{2,}/.test(lines[i + 1] || '')) {   // tabella
    const rows = [splitRow(line)];
    i += 2;
    while (i < lines.length && /^\|/.test(lines[i])) rows.push(splitRow(lines[i++]));
    children.push(makeTable(rows));
    children.push(new Paragraph({ text: '', spacing: { after: 120 } }));
    continue;
  }

  if (/^#{1,4} /.test(line)) {                        // titolo
    const level = line.match(/^#+/)[0].length;
    const text = line.replace(/^#+\s*/, '');
    if (level === 1) {
      children.push(new Paragraph({
        children: runs(text), heading: HeadingLevel.TITLE, alignment: AlignmentType.CENTER,
        spacing: { after: 240 },
      }));
    } else {
      children.push(new Paragraph({
        children: runs(text),
        heading: [null, null, HeadingLevel.HEADING_1, HeadingLevel.HEADING_2, HeadingLevel.HEADING_3][level],
        spacing: { before: level === 2 ? 320 : 240, after: 120 },
        pageBreakBefore: level === 2 && children.length > 6,
      }));
    }
    i++;
    continue;
  }

  if (/^---+\s*$/.test(line)) { i++; continue; }      // regola orizzontale: ignorata

  if (/^\s*[-*] /.test(line)) {                       // elenco puntato
    const indent = line.search(/\S/);
    children.push(new Paragraph({
      children: runs(line.replace(/^\s*[-*]\s+/, '')),
      numbering: { reference: 'punti', level: indent >= 2 ? 1 : 0 },
      spacing: { before: 40, after: 40 },
    }));
    i++;
    continue;
  }

  if (/^\s*\d+\. /.test(line)) {                      // elenco numerato
    children.push(new Paragraph({
      children: runs(line.replace(/^\s*\d+\.\s+/, '')),
      numbering: { reference: 'numeri', level: 0 },
      spacing: { before: 40, after: 40 },
    }));
    i++;
    continue;
  }

  if (!line.trim()) { i++; continue; }

  const buf = [line];                                 // paragrafo (righe contigue)
  i++;
  while (i < lines.length && lines[i].trim() && !/^(#{1,4} |```|\||\s*[-*] |\s*\d+\. |---)/.test(lines[i]))
    buf.push(lines[i++]);
  children.push(new Paragraph({
    children: runs(buf.join(' ')),
    spacing: { before: 60, after: 60, line: 276 },
  }));
}

const frontmatter = [
  new Paragraph({ text: '', spacing: { after: 240 } }),
  new Paragraph({
    children: [new TextRun({ text: 'Indice', bold: true, size: 28 })],
    spacing: { after: 120 },
  }),
  new Paragraph({
    children: [new TextRun({
      text: 'In Word: clic destro sull\'indice \u2192 Aggiorna campo, per popolarlo.',
      italics: true, size: 18, color: '666666',
    })],
    spacing: { after: 160 },
  }),
  new TableOfContents('Indice', { hyperlink: true, headingStyleRange: '1-3' }),
  new Paragraph({ children: [new PageBreak()] }),
];

const doc = new Document({
  creator: 'ShashChess porting toolkit',
  title: 'ShashChess 42 - procedura di allineamento a Stockfish 19',
  numbering: {
    config: [
      {
        reference: 'punti',
        levels: [
          { level: 0, format: LevelFormat.BULLET, text: '\u2022', alignment: AlignmentType.LEFT,
            style: { paragraph: { indent: { left: convertInchesToTwip(0.3), hanging: convertInchesToTwip(0.18) } } } },
          { level: 1, format: LevelFormat.BULLET, text: '\u25E6', alignment: AlignmentType.LEFT,
            style: { paragraph: { indent: { left: convertInchesToTwip(0.6), hanging: convertInchesToTwip(0.18) } } } },
        ],
      },
      {
        reference: 'numeri',
        levels: [
          { level: 0, format: LevelFormat.DECIMAL, text: '%1.', alignment: AlignmentType.LEFT,
            style: { paragraph: { indent: { left: convertInchesToTwip(0.3), hanging: convertInchesToTwip(0.18) } } } },
        ],
      },
    ],
  },
  styles: {
    default: {
      document: { run: { font: 'Calibri', size: 21 } },
      heading1: { run: { font: 'Calibri', size: 30, bold: true, color: '1F3864' } },
      heading2: { run: { font: 'Calibri', size: 25, bold: true, color: '2E5496' } },
      heading3: { run: { font: 'Calibri', size: 22, bold: true, color: '333333' } },
      title: { run: { font: 'Calibri', size: 40, bold: true, color: '1F3864' } },
    },
  },
  sections: [{
    properties: { page: { size: { width: 12240, height: 15840 }, margin: { top: 1080, bottom: 1080, left: 1440, right: 1440 } } },
    children: [...children.slice(0, 1), ...frontmatter, ...children.slice(1)],
  }],
});

Packer.toBuffer(doc).then(b => { fs.writeFileSync(OUT, b); console.log('scritto ' + OUT + ' (' + b.length + ' byte)'); });
