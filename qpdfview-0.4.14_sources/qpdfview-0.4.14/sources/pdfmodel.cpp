/*

Copyright 2014 S. Razi Alavizadeh
Copyright 2013-2014 Adam Reichold

This file is part of qpdfview.

qpdfview is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

qpdfview is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with qpdfview.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "pdfmodel.h"

#include <QCache>
#include <QFormLayout>
#include <QMessageBox>
#include <QSettings>

#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)

#include <poppler-qt5.h>

#else

#include <poppler-qt4.h>

#endif // QT_VERSION

#include <poppler-form.h>

#include "annotationwidgets.h"
#include "formfieldwidgets.h"

#ifndef HAS_POPPLER_24

#define LOCK_ANNOTATION QMutexLocker mutexLocker(m_mutex);
#define LOCK_FORM_FIELD QMutexLocker mutexLocker(m_mutex);
#define LOCK_PAGE QMutexLocker mutexLocker(m_mutex);
#define LOCK_DOCUMENT QMutexLocker mutexLocker(&m_mutex);

#else

#define LOCK_ANNOTATION
#define LOCK_FORM_FIELD
#define LOCK_PAGE
#define LOCK_DOCUMENT

#endif // HAS_POPPLER_24

namespace
{

using namespace qpdfview;
using namespace qpdfview::Model;

void loadOutline(Poppler::Document* document, const QDomNode& node, QStandardItem* parent)
{
    const QDomElement element = node.toElement();

    QStandardItem* item = new QStandardItem(element.tagName());
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

    Poppler::LinkDestination* linkDestination = 0;

    if(element.hasAttribute("Destination"))
    {
        linkDestination = new Poppler::LinkDestination(element.attribute("Destination"));
    }
    else if(element.hasAttribute("DestinationName"))
    {
        linkDestination = document->linkDestination(element.attribute("DestinationName"));
    }

    if(linkDestination != 0)
    {
        int page = linkDestination->pageNumber();
        qreal left = 0.0;
        qreal top = 0.0;

        page = page >= 1 ? page : 1;
        page = page <= document->numPages() ? page : document->numPages();

        if(linkDestination->isChangeLeft())
        {
            left = linkDestination->left();

            left = left >= 0.0 ? left : 0.0;
            left = left <= 1.0 ? left : 1.0;
        }

        if(linkDestination->isChangeTop())
        {
            top = linkDestination->top();

            top = top >= 0.0 ? top : 0.0;
            top = top <= 1.0 ? top : 1.0;
        }

        delete linkDestination;

        item->setData(page, Document::PageRole);
        item->setData(left, Document::LeftRole);
        item->setData(top, Document::TopRole);

        QStandardItem* pageItem = item->clone();
        pageItem->setText(QString::number(page));
        pageItem->setTextAlignment(Qt::AlignRight);

        parent->appendRow(QList< QStandardItem* >() << item << pageItem);
    }
    else
    {
        parent->appendRow(item);
    }

    const QDomNode& siblingNode = node.nextSibling();
    if(!siblingNode.isNull())
    {
        loadOutline(document, siblingNode, parent);
    }

    const QDomNode& childNode = node.firstChild();
    if(!childNode.isNull())
    {
        loadOutline(document, childNode, item);
    }
}

inline void restoreRenderHint(Poppler::Document* document, const Poppler::Document::RenderHints hints, const Poppler::Document::RenderHint hint)
{
    document->setRenderHint(hint, hints.testFlag(hint));
}

typedef QSharedPointer< Poppler::TextBox > TextBox;
typedef QList< TextBox > TextBoxList;

QCache< const PdfPage*,  TextBoxList > textCache(1 << 12);
QMutex textCacheMutex;

#define LOCK_TEXT_CACHE QMutexLocker mutexLocker(&textCacheMutex);

} // anonymous

namespace qpdfview
{

namespace Model
{

PdfAnnotation::PdfAnnotation(QMutex* mutex, Poppler::Annotation* annotation) : Annotation(),
    m_mutex(mutex),
    m_annotation(annotation)
{
}

PdfAnnotation::~PdfAnnotation()
{
    delete m_annotation;
}

QRectF PdfAnnotation::boundary() const
{
    LOCK_ANNOTATION

    return m_annotation->boundary().normalized();
}

QString PdfAnnotation::contents() const
{
    LOCK_ANNOTATION

    return m_annotation->contents();
}

QWidget* PdfAnnotation::createWidget()
{
    QWidget* widget = 0;

    if(m_annotation->subType() == Poppler::Annotation::AText || m_annotation->subType() == Poppler::Annotation::AHighlight)
    {
        widget = new AnnotationWidget(m_mutex, m_annotation);

        connect(widget, SIGNAL(wasModified()), SIGNAL(wasModified()));
    }
    else if(m_annotation->subType() == Poppler::Annotation::AFileAttachment)
    {
        widget = new FileAttachmentAnnotationWidget(m_mutex, static_cast< Poppler::FileAttachmentAnnotation* >(m_annotation));
    }

    connect(this, SIGNAL(destroyed()), widget, SLOT(deleteLater()));

    return widget;
}

PdfFormField::PdfFormField(QMutex* mutex, Poppler::FormField* formField) : FormField(),
    m_mutex(mutex),
    m_formField(formField)
{
}

PdfFormField::~PdfFormField()
{
    delete m_formField;
}

QRectF PdfFormField::boundary() const
{
    LOCK_FORM_FIELD

    return m_formField->rect().normalized();
}

QString PdfFormField::name() const
{
    LOCK_FORM_FIELD

    return m_formField->name();
}

QWidget* PdfFormField::createWidget()
{
    QWidget* widget = 0;

    if(m_formField->type() == Poppler::FormField::FormText)
    {
        Poppler::FormFieldText* formFieldText = static_cast< Poppler::FormFieldText* >(m_formField);

        if(formFieldText->textType() == Poppler::FormFieldText::Normal)
        {
            widget = new NormalTextFieldWidget(m_mutex, formFieldText);
        }
        else if(formFieldText->textType() == Poppler::FormFieldText::Multiline)
        {
            widget = new MultilineTextFieldWidget(m_mutex, formFieldText);
        }
    }
    else if(m_formField->type() == Poppler::FormField::FormChoice)
    {
        Poppler::FormFieldChoice* formFieldChoice = static_cast< Poppler::FormFieldChoice* >(m_formField);

        if(formFieldChoice->choiceType() == Poppler::FormFieldChoice::ComboBox)
        {
            widget = new ComboBoxChoiceFieldWidget(m_mutex, formFieldChoice);
        }
        else if(formFieldChoice->choiceType() == Poppler::FormFieldChoice::ListBox)
        {
            widget = new ListBoxChoiceFieldWidget(m_mutex, formFieldChoice);
        }
    }
    else if(m_formField->type() == Poppler::FormField::FormButton)
    {
        Poppler::FormFieldButton* formFieldButton = static_cast< Poppler::FormFieldButton* >(m_formField);

        if(formFieldButton->buttonType() == Poppler::FormFieldButton::CheckBox)
        {
            widget = new CheckBoxChoiceFieldWidget(m_mutex, formFieldButton);
        }
        else if(formFieldButton->buttonType() == Poppler::FormFieldButton::Radio)
        {
            widget = new RadioChoiceFieldWidget(m_mutex, formFieldButton);
        }
    }

    connect(widget, SIGNAL(wasModified()), SIGNAL(wasModified()));

    return widget;
}

PdfPage::PdfPage(QMutex* mutex, Poppler::Page* page) :
    m_mutex(mutex),
    m_page(page)
{
}

PdfPage::~PdfPage()
{
    {
        LOCK_TEXT_CACHE

        textCache.remove(this);
    }

    delete m_page;
}

QSizeF PdfPage::size() const
{
    LOCK_PAGE

    return m_page->pageSizeF();
}

QImage PdfPage::render(qreal horizontalResolution, qreal verticalResolution, Rotation rotation, const QRect& boundingRect) const
{
    LOCK_PAGE

    Poppler::Page::Rotation rotate;

    switch(rotation)
    {
    default:
    case RotateBy0:
        rotate = Poppler::Page::Rotate0;
        break;
    case RotateBy90:
        rotate = Poppler::Page::Rotate90;
        break;
    case RotateBy180:
        rotate = Poppler::Page::Rotate180;
        break;
    case RotateBy270:
        rotate = Poppler::Page::Rotate270;
        break;
    }

    int x = -1;
    int y = -1;
    int w = -1;
    int h = -1;

    if(!boundingRect.isNull())
    {
        x = boundingRect.x();
        y = boundingRect.y();
        w = boundingRect.width();
        h = boundingRect.height();
    }

    return m_page->renderToImage(horizontalResolution, verticalResolution, x, y, w, h, rotate);
}

QString PdfPage::label() const
{
    LOCK_PAGE

    return m_page->label();
}

QList< Link* > PdfPage::links() const
{
    LOCK_PAGE

    QList< Link* > links;

    foreach(const Poppler::Link* link, m_page->links())
    {
        const QRectF boundary = link->linkArea().normalized();

        if(link->linkType() == Poppler::Link::Goto)
        {
            const Poppler::LinkGoto* linkGoto = static_cast< const Poppler::LinkGoto* >(link);

            const int page = linkGoto->destination().pageNumber();

            qreal left = linkGoto->destination().isChangeLeft() ? linkGoto->destination().left() : 0.0;
            qreal top = linkGoto->destination().isChangeTop() ? linkGoto->destination().top() : 0.0;

            left = left >= 0.0 ? left : 0.0;
            left = left <= 1.0 ? left : 1.0;

            top = top >= 0.0 ? top : 0.0;
            top = top <= 1.0 ? top : 1.0;

            if(linkGoto->isExternal())
            {
                links.append(new Link(boundary, linkGoto->fileName(), page));
            }
            else
            {
                links.append(new Link(boundary, page, left, top));
            }
        }
        else if(link->linkType() == Poppler::Link::Browse)
        {
            const Poppler::LinkBrowse* linkBrowse = static_cast< const Poppler::LinkBrowse* >(link);
            const QString url = linkBrowse->url();

            links.append(new Link(boundary, url));
        }
        else if(link->linkType() == Poppler::Link::Execute)
        {
            const Poppler::LinkExecute* linkExecute = static_cast< const Poppler::LinkExecute* >(link);
            const QString url = linkExecute->fileName();

            links.append(new Link(boundary, url));
        }

        delete link;
    }

    return links;
}

QString PdfPage::text(const QRectF& rect) const
{
    LOCK_PAGE

    return m_page->text(rect).simplified();
}

QString PdfPage::cachedText(const QRectF& rect) const
{
    bool wasCached = false;
    TextBoxList textBoxes;

    {
        LOCK_TEXT_CACHE

        if(const TextBoxList* object = textCache.object(this))
        {
            wasCached = true;

            textBoxes = *object;
        }
    }

    if(!wasCached)
    {
        {
            LOCK_PAGE

            foreach(Poppler::TextBox* textBox, m_page->textList())
            {
                textBoxes.append(TextBox(textBox));
            }
        }

        LOCK_TEXT_CACHE

        textCache.insert(this, new TextBoxList(textBoxes), textBoxes.count());
    }

    QString text;

    foreach(const TextBox& textBox, textBoxes)
    {
        if(!rect.intersects(textBox->boundingBox()))
        {
            continue;
        }

        const QString& characters = textBox->text();

        for(int index = 0; index < characters.length(); ++index)
        {
            if(rect.intersects(textBox->charBoundingBox(index)))
            {
                text.append(characters.at(index));
            }
        }

        if(textBox->hasSpaceAfter())
        {
            text.append(QLatin1Char(' '));
        }
    }

    return text.simplified();
}

QList< QRectF > PdfPage::search(const QString& text, bool matchCase, bool wholeWords) const
{
    LOCK_PAGE

    QList< QRectF > results;

#if defined(HAS_POPPLER_31)

    const Poppler::Page::SearchFlags flags((matchCase ? 0 : Poppler::Page::IgnoreCase) | (wholeWords ? Poppler::Page::WholeWords : 0));

    results = m_page->search(text, flags);

#elif defined(HAS_POPPLER_22)

    const Poppler::Page::SearchMode mode = matchCase ? Poppler::Page::CaseSensitive : Poppler::Page::CaseInsensitive;

    results = m_page->search(text, mode);

#elif defined(HAS_POPPLER_14)

    const Poppler::Page::SearchMode mode = matchCase ? Poppler::Page::CaseSensitive : Poppler::Page::CaseInsensitive;

    double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;

    while(m_page->search(text, left, top, right, bottom, Poppler::Page::NextResult, mode))
    {
        results.append(QRectF(left, top, right - left, bottom - top));
    }

#else

    const Poppler::Page::SearchMode mode = matchCase ? Poppler::Page::CaseSensitive : Poppler::Page::CaseInsensitive;

    QRectF rect;

    while(m_page->search(text, rect, Poppler::Page::NextResult, mode))
    {
        results.append(rect);
    }

#endif // HAS_POPPLER_31 HAS_POPPLER_22 HAS_POPPLER_14

    return results;
}

QList< Annotation* > PdfPage::annotations() const
{
    LOCK_PAGE

    QList< Annotation* > annotations;

    foreach(Poppler::Annotation* annotation, m_page->annotations())
    {
        if(annotation->subType() == Poppler::Annotation::AText || annotation->subType() == Poppler::Annotation::AHighlight || annotation->subType() == Poppler::Annotation::AFileAttachment)
        {
            annotations.append(new PdfAnnotation(m_mutex, annotation));
            continue;
        }

        delete annotation;
    }

    return annotations;
}

bool PdfPage::canAddAndRemoveAnnotations() const
{
#ifdef HAS_POPPLER_20

    return true;

#else

    QMessageBox::information(0, tr("Information"), tr("Version 0.20.1 or higher of the Poppler library is required to add or remove annotations."));

    return false;

#endif // HAS_POPPLER_20
}

Annotation* PdfPage::addTextAnnotation(const QRectF& boundary, const QColor& color)
{
    LOCK_PAGE

#ifdef HAS_POPPLER_20

    Poppler::Annotation::Style style;
    style.setColor(color);

    Poppler::Annotation::Popup popup;
    popup.setFlags(Poppler::Annotation::Hidden | Poppler::Annotation::ToggleHidingOnMouse);

    Poppler::Annotation* annotation = new Poppler::TextAnnotation(Poppler::TextAnnotation::Linked);

    annotation->setBoundary(boundary);
    annotation->setStyle(style);
    annotation->setPopup(popup);

    m_page->addAnnotation(annotation);

    return new PdfAnnotation(m_mutex, annotation);

#else

    Q_UNUSED(boundary);
    Q_UNUSED(color);

    return 0;

#endif // HAS_POPPLER_20
}

Annotation* PdfPage::addHighlightAnnotation(const QRectF& boundary, const QColor& color)
{
    LOCK_PAGE

#ifdef HAS_POPPLER_20

    Poppler::Annotation::Style style;
    style.setColor(color);

    Poppler::Annotation::Popup popup;
    popup.setFlags(Poppler::Annotation::Hidden | Poppler::Annotation::ToggleHidingOnMouse);

    Poppler::HighlightAnnotation* annotation = new Poppler::HighlightAnnotation();

    Poppler::HighlightAnnotation::Quad quad;
    quad.points[0] = boundary.topLeft();
    quad.points[1] = boundary.topRight();
    quad.points[2] = boundary.bottomRight();
    quad.points[3] = boundary.bottomLeft();

    annotation->setHighlightQuads(QList< Poppler::HighlightAnnotation::Quad >() << quad);

    annotation->setBoundary(boundary);
    annotation->setStyle(style);
    annotation->setPopup(popup);

    m_page->addAnnotation(annotation);

    return new PdfAnnotation(m_mutex, annotation);

#else

    Q_UNUSED(boundary);
    Q_UNUSED(color);

    return 0;

#endif // HAS_POPPLER_20

}

void PdfPage::removeAnnotation(Annotation* annotation)
{
    LOCK_PAGE

#ifdef HAS_POPPLER_20

    PdfAnnotation* pdfAnnotation = static_cast< PdfAnnotation* >(annotation);

    m_page->removeAnnotation(pdfAnnotation->m_annotation);
    pdfAnnotation->m_annotation = 0;

#else

    Q_UNUSED(annotation);

#endif // HAS_POPPLER_20
}

QList< FormField* > PdfPage::formFields() const
{
    LOCK_PAGE

    QList< FormField* > formFields;

    foreach(Poppler::FormField* formField, m_page->formFields())
    {
        if(!formField->isVisible() || formField->isReadOnly())
        {
            delete formField;
            continue;
        }

        if(formField->type() == Poppler::FormField::FormText)
        {
            Poppler::FormFieldText* formFieldText = static_cast< Poppler::FormFieldText* >(formField);

            if(formFieldText->textType() == Poppler::FormFieldText::Normal || formFieldText->textType() == Poppler::FormFieldText::Multiline)
            {
                formFields.append(new PdfFormField(m_mutex, formField));
                continue;
            }
        }
        else if(formField->type() == Poppler::FormField::FormChoice)
        {
            Poppler::FormFieldChoice* formFieldChoice = static_cast< Poppler::FormFieldChoice* >(formField);

            if(formFieldChoice->choiceType() == Poppler::FormFieldChoice::ListBox || formFieldChoice->choiceType() == Poppler::FormFieldChoice::ComboBox)
            {
                formFields.append(new PdfFormField(m_mutex, formField));
                continue;
            }
        }
        else if(formField->type() == Poppler::FormField::FormButton)
        {
            Poppler::FormFieldButton* formFieldButton = static_cast< Poppler::FormFieldButton* >(formField);

            if(formFieldButton->buttonType() == Poppler::FormFieldButton::CheckBox || formFieldButton->buttonType() == Poppler::FormFieldButton::Radio)
            {
                formFields.append(new PdfFormField(m_mutex, formField));
                continue;
            }
        }

        delete formField;
    }

    return formFields;
}

PdfDocument::PdfDocument(Poppler::Document* document) :
    m_mutex(),
    m_document(document)
{
}

PdfDocument::~PdfDocument()
{
    delete m_document;
}

int PdfDocument::numberOfPages() const
{
    LOCK_DOCUMENT

    return m_document->numPages();
}

Page* PdfDocument::page(int index) const
{
    LOCK_DOCUMENT

    Poppler::Page* page = m_document->page(index);

    return page != 0 ? new PdfPage(&m_mutex, page) : 0;
}

bool PdfDocument::isLocked() const
{
    LOCK_DOCUMENT

    return m_document->isLocked();
}

bool PdfDocument::unlock(const QString& password)
{
    LOCK_DOCUMENT

    // Poppler drops render hints and backend after unlocking so we need to restore them.

    const Poppler::Document::RenderHints hints = m_document->renderHints();
    const Poppler::Document::RenderBackend backend = m_document->renderBackend();

    const bool ok = m_document->unlock(password.toLatin1(), password.toLatin1());

    restoreRenderHint(m_document, hints, Poppler::Document::Antialiasing);
    restoreRenderHint(m_document, hints, Poppler::Document::TextAntialiasing);

#ifdef HAS_POPPLER_14

    restoreRenderHint(m_document, hints, Poppler::Document::TextHinting);

#endif // HAS_POPPLER_18 HAS_POPPLER_14

#ifdef HAS_POPPLER_18

    restoreRenderHint(m_document, hints, Poppler::Document::TextSlightHinting);

#endif // HAS_POPPLER_18

#ifdef HAS_POPPLER_22

    restoreRenderHint(m_document, hints, Poppler::Document::OverprintPreview);

#endif // HAS_POPPLER_22

#ifdef HAS_POPPLER_24

    restoreRenderHint(m_document, hints, Poppler::Document::ThinLineSolid);
    restoreRenderHint(m_document, hints, Poppler::Document::ThinLineShape);

#endif // HAS_POPPLER_24

    m_document->setRenderBackend(backend);

    return ok;
}

QStringList PdfDocument::saveFilter() const
{
    return QStringList() << "Portable document format (*.pdf)";
}

bool PdfDocument::canSave() const
{
    return true;
}

bool PdfDocument::save(const QString& filePath, bool withChanges) const
{
    LOCK_DOCUMENT

    QScopedPointer< Poppler::PDFConverter > pdfConverter(m_document->pdfConverter());

    pdfConverter->setOutputFileName(filePath);

    if(withChanges)
    {
        pdfConverter->setPDFOptions(pdfConverter->pdfOptions() | Poppler::PDFConverter::WithChanges);
    }

    return pdfConverter->convert();
}

bool PdfDocument::canBePrintedUsingCUPS() const
{
    return true;
}

void PdfDocument::setPaperColor(const QColor& paperColor)
{
    LOCK_DOCUMENT

    m_document->setPaperColor(paperColor);
}

void PdfDocument::loadOutline(QStandardItemModel* outlineModel) const
{
    Document::loadOutline(outlineModel);

    LOCK_DOCUMENT

    QDomDocument* toc = m_document->toc();

    if(toc != 0)
    {
        ::loadOutline(m_document, toc->firstChild(), outlineModel->invisibleRootItem());

        delete toc;
    }
}

void PdfDocument::loadProperties(QStandardItemModel* propertiesModel) const
{
    Document::loadProperties(propertiesModel);

    LOCK_DOCUMENT

    QStringList keys = m_document->infoKeys();

    propertiesModel->setRowCount(keys.count());
    propertiesModel->setColumnCount(2);

    for(int index = 0; index < keys.count(); ++index)
    {
        const QString key = keys.at(index);
        QString value = m_document->info(key);

        if(value.startsWith("D:"))
        {
            value = m_document->date(key).toString();
        }

        propertiesModel->setItem(index, 0, new QStandardItem(key));
        propertiesModel->setItem(index, 1, new QStandardItem(value));
    }
}

void PdfDocument::loadFonts(QStandardItemModel* fontsModel) const
{
    Document::loadFonts(fontsModel);

    LOCK_DOCUMENT

    const QList< Poppler::FontInfo > fonts = m_document->fonts();

    fontsModel->setRowCount(fonts.count());
    fontsModel->setColumnCount(5);

    fontsModel->setHorizontalHeaderLabels(QStringList() << tr("Name") << tr("Type") << tr("Embedded") << tr("Subset") << tr("File"));

    for(int index = 0; index < fonts.count(); ++index)
    {
        const Poppler::FontInfo& font = fonts[index];

        fontsModel->setItem(index, 0, new QStandardItem(font.name()));
        fontsModel->setItem(index, 1, new QStandardItem(font.typeName()));
        fontsModel->setItem(index, 2, new QStandardItem(font.isEmbedded() ? tr("Yes") : tr("No")));
        fontsModel->setItem(index, 3, new QStandardItem(font.isSubset() ? tr("Yes") : tr("No")));
        fontsModel->setItem(index, 4, new QStandardItem(font.file()));
    }
}

bool PdfDocument::wantsContinuousMode() const
{
    LOCK_DOCUMENT

    const Poppler::Document::PageLayout pageLayout = m_document->pageLayout();

    return pageLayout == Poppler::Document::OneColumn
        || pageLayout == Poppler::Document::TwoColumnLeft
        || pageLayout == Poppler::Document::TwoColumnRight;
}

bool PdfDocument::wantsSinglePageMode() const
{
    LOCK_DOCUMENT

    const Poppler::Document::PageLayout pageLayout = m_document->pageLayout();

    return pageLayout == Poppler::Document::SinglePage
        || pageLayout == Poppler::Document::OneColumn;
}

bool PdfDocument::wantsTwoPagesMode() const
{
    LOCK_DOCUMENT

    const Poppler::Document::PageLayout pageLayout = m_document->pageLayout();

    return pageLayout == Poppler::Document::TwoPageLeft
        || pageLayout == Poppler::Document::TwoColumnLeft;
}

bool PdfDocument::wantsTwoPagesWithCoverPageMode() const
{
    LOCK_DOCUMENT

    const Poppler::Document::PageLayout pageLayout = m_document->pageLayout();

    return pageLayout == Poppler::Document::TwoPageRight
        || pageLayout == Poppler::Document::TwoColumnRight;
}

bool PdfDocument::wantsRightToLeftMode() const
{
#ifdef HAS_POPPLER_26

    return m_document->textDirection() == Qt::RightToLeft;

#else

    return false;

#endif // HAS_POPPLER_26
}

} // Model

PdfSettingsWidget::PdfSettingsWidget(QSettings* settings, QWidget* parent) : SettingsWidget(parent),
    m_settings(settings)
{
    m_layout = new QFormLayout(this);

    // antialiasing

    m_antialiasingCheckBox = new QCheckBox(this);
    m_antialiasingCheckBox->setChecked(m_settings->value("antialiasing", true).toBool());

    m_layout->addRow(tr("Antialiasing:"), m_antialiasingCheckBox);

    // text antialising

    m_textAntialiasingCheckBox = new QCheckBox(this);
    m_textAntialiasingCheckBox->setChecked(m_settings->value("textAntialiasing", true).toBool());

    m_layout->addRow(tr("Text antialiasing:"), m_textAntialiasingCheckBox);

    // text hinting

#ifdef HAS_POPPLER_18

    m_textHintingComboBox = new QComboBox(this);
    m_textHintingComboBox->addItem(tr("None"));
    m_textHintingComboBox->addItem(tr("Full"));
    m_textHintingComboBox->addItem(tr("Reduced"));
    m_textHintingComboBox->setCurrentIndex(m_settings->value("textHinting", 0).toInt());

    m_layout->addRow(tr("Text hinting:"), m_textHintingComboBox);

#else

    m_textHintingCheckBox = new QCheckBox(this);
    m_textHintingCheckBox->setChecked(m_settings->value("textHinting", false).toBool());

    m_layout->addRow(tr("Text hinting:"), m_textHintingCheckBox);

#endif // HAS_POPPLER_18

#ifdef HAS_POPPLER_22

    // overprint preview

    m_overprintPreviewCheckBox = new QCheckBox(this);
    m_overprintPreviewCheckBox->setChecked(m_settings->value("overprintPreview", false).toBool());

    m_layout->addRow(tr("Overprint preview:"), m_overprintPreviewCheckBox);

#endif // HAS_POPPLER_22

#ifdef HAS_POPPLER_24

    m_thinLineModeComboBox = new QComboBox(this);
    m_thinLineModeComboBox->addItem(tr("None"));
    m_thinLineModeComboBox->addItem(tr("Solid"));
    m_thinLineModeComboBox->addItem(tr("Shaped"));
    m_thinLineModeComboBox->setCurrentIndex(m_settings->value("thinLineMode", 0).toInt());

    m_layout->addRow(tr("Thin line mode:"), m_thinLineModeComboBox);

#endif // HAS_POPPLER_24

    m_backendComboBox = new QComboBox(this);
    m_backendComboBox->addItem(tr("Splash"));
    m_backendComboBox->addItem(tr("Arthur"));
    m_backendComboBox->setCurrentIndex(m_settings->value("backend", 0).toInt());

    m_layout->addRow(tr("Backend:"), m_backendComboBox);
}

void PdfSettingsWidget::accept()
{
    m_settings->setValue("antialiasing", m_antialiasingCheckBox->isChecked());
    m_settings->setValue("textAntialiasing", m_textAntialiasingCheckBox->isChecked());

#ifdef HAS_POPPLER_18

    m_settings->setValue("textHinting", m_textHintingComboBox->currentIndex());

#else

    m_settings->setValue("textHinting", m_textHintingCheckBox->isChecked());

#endif // HAS_POPPLER_18


#ifdef HAS_POPPLER_22

    m_settings->setValue("overprintPreview", m_overprintPreviewCheckBox->isChecked());

#endif // HAS_POPPLER_22

#ifdef HAS_POPPLER_24

    m_settings->setValue("thinLineMode", m_thinLineModeComboBox->currentIndex());

#endif // HAS_POPPLER_24

    m_settings->setValue("backend", m_backendComboBox->currentIndex());
}

void PdfSettingsWidget::reset()
{
    m_antialiasingCheckBox->setChecked(true);
    m_textAntialiasingCheckBox->setChecked(true);

#ifdef HAS_POPPLER_18

    m_textHintingComboBox->setCurrentIndex(0);

#else

    m_textHintingCheckBox->setChecked(false);

#endif // HAS_POPPLER_18

#ifdef HAS_POPPLER_22

    m_overprintPreviewCheckBox->setChecked(false);

#endif // HAS_POPPLER_22

#ifdef HAS_POPPLER_24

    m_thinLineModeComboBox->setCurrentIndex(0);

#endif // HAS_POPPLER_24

    m_backendComboBox->setCurrentIndex(0);
}

PdfPlugin::PdfPlugin(QObject* parent) : QObject(parent)
{
    setObjectName("PdfPlugin");

    m_settings = new QSettings("qpdfview", "pdf-plugin", this);
}

Model::Document* PdfPlugin::loadDocument(const QString& filePath) const
{
    Poppler::Document* document = Poppler::Document::load(filePath);

    if(document != 0)
    {
        document->setRenderHint(Poppler::Document::Antialiasing, m_settings->value("antialiasing", true).toBool());
        document->setRenderHint(Poppler::Document::TextAntialiasing, m_settings->value("textAntialiasing", true).toBool());

#if defined(HAS_POPPLER_18)

        switch(m_settings->value("textHinting", 0).toInt())
        {
        default:
        case 0:
            document->setRenderHint(Poppler::Document::TextHinting, false);
            break;
        case 1:
            document->setRenderHint(Poppler::Document::TextHinting, true);
            document->setRenderHint(Poppler::Document::TextSlightHinting, false);
            break;
        case 2:
            document->setRenderHint(Poppler::Document::TextHinting, true);
            document->setRenderHint(Poppler::Document::TextSlightHinting, true);
            break;
        }

#elif defined(HAS_POPPLER_14)

        document->setRenderHint(Poppler::Document::TextHinting, m_settings->value("textHinting", false).toBool());

#endif // HAS_POPPLER_18 HAS_POPPLER_14

#ifdef HAS_POPPLER_22

        document->setRenderHint(Poppler::Document::OverprintPreview, m_settings->value("overprintPreview", false).toBool());

#endif // HAS_POPPLER_22

#ifdef HAS_POPPLER_24

        switch(m_settings->value("thinLineMode", 0).toInt())
        {
        default:
        case 0:
            document->setRenderHint(Poppler::Document::ThinLineSolid, false);
            document->setRenderHint(Poppler::Document::ThinLineShape, false);
            break;
        case 1:
            document->setRenderHint(Poppler::Document::ThinLineSolid, true);
            document->setRenderHint(Poppler::Document::ThinLineShape, false);
            break;
        case 2:
            document->setRenderHint(Poppler::Document::ThinLineSolid, false);
            document->setRenderHint(Poppler::Document::ThinLineShape, true);
            break;
        }

#endif // HAS_POPPLER_24

        switch(m_settings->value("backend").toInt())
        {
        default:
        case 0:
            document->setRenderBackend(Poppler::Document::SplashBackend);
            break;
        case 1:
            document->setRenderBackend(Poppler::Document::ArthurBackend);
            break;
        }
    }

    return document != 0 ? new Model::PdfDocument(document) : 0;
}

SettingsWidget* PdfPlugin::createSettingsWidget(QWidget* parent) const
{
    return new PdfSettingsWidget(m_settings, parent);
}

} // qpdfview

#if QT_VERSION < QT_VERSION_CHECK(5,0,0)

Q_EXPORT_PLUGIN2(qpdfview_pdf, qpdfview::PdfPlugin)

#endif // QT_VERSION
