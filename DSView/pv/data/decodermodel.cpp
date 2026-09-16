/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <libsigrokdecode.h>
  
#include "decode/annotation.h"
#include "decode/rowdata.h"
#include "decoderstack.h"
#include "decodermodel.h"
#include "../log.h"
#include <ds_types.h>

using namespace boost;
using namespace std;

namespace pv {
namespace data {

DecoderModel::DecoderModel(QObject *parent)
    : QAbstractTableModel(parent),
      _decoder_stack(NULL)
{
}

void DecoderModel::setDecoderStack(DecoderStack *decoder_stack)
{
    beginResetModel();
    _decoder_stack = decoder_stack;
    endResetModel();
}

void DecoderModel::refresh()
{
    beginResetModel();
    endResetModel();
    if (_decoder_stack && _decoder_stack->has_spi_message_history())
        dsv_info("Protocol table refresh: SPI messages=%llu",
            (u64_t)_decoder_stack->spi_message_count());
    else if (_decoder_stack && _decoder_stack->has_uart_message_history())
        dsv_info("Protocol table refresh: UART messages=%llu",
            (u64_t)_decoder_stack->uart_message_count());
}
 
int DecoderModel::rowCount(const QModelIndex & /* parent */) const
{
    if (_decoder_stack) {
        if (_decoder_stack->has_spi_message_history())
            return _decoder_stack->spi_message_count();
        if (_decoder_stack->has_uart_message_history())
            return _decoder_stack->uart_message_count();
    }
    if (_decoder_stack)
        return _decoder_stack->list_annotation_size();
    else
        return 100;
}
int DecoderModel::columnCount(const QModelIndex & /* parent */) const
{
    if (_decoder_stack) {
        if (_decoder_stack->has_spi_message_history())
            return 3;
        if (_decoder_stack->has_uart_message_history())
            return 2;
    }
    if (_decoder_stack)
        return _decoder_stack->list_rows_size();
    else
        return 1;
}

QVariant DecoderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::TextAlignmentRole) {
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    else if (role == Qt::DisplayRole) {
        if (_decoder_stack) {
            if (_decoder_stack->has_spi_message_history()) {
                DecoderStack::SpiMessage message;
                if (_decoder_stack->spi_message(index.row(), message)) {
                    if (index.column() == 0)
                        return message.timestamp;
                    return index.column() == 1 ? message.miso : message.mosi;
                }
                return QVariant();
            }
            if (_decoder_stack->has_uart_message_history()) {
                DecoderStack::UartMessage message;
                if (_decoder_stack->uart_message(index.row(), message))
                    return index.column() == 0 ? message.timestamp : message.rxtx;
                return QVariant();
            }
            pv::data::decode::Annotation ann;
            if (_decoder_stack->list_annotation(&ann, index.column(), index.row())) {
                return ann.annotations().at(0);
            }
        }
    }
    return QVariant();
}

QVariant DecoderModel::headerData(int section,
                                   Qt::Orientation  orientation,
                                   int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Vertical)
        return section;

    if (_decoder_stack) {
        if (_decoder_stack->has_spi_message_history()) {
            static const char *const titles[] = {"Date", "MISO", "MOSI"};
            return section >= 0 && section < 3 ? titles[section] : QVariant();
        }
        if (_decoder_stack->has_uart_message_history()) {
            static const char *const titles[] = {"Date", "RX/TX"};
            return section >= 0 && section < 2 ? titles[section] : QVariant();
        }
        QString title;
        if (_decoder_stack->list_row_title(section, title))
            return title;
    }
    return QVariant();
}

} // namespace data
} // namespace pv
