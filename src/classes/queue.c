/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Thomas Punt <tpunt@php.net>                                  |
  +----------------------------------------------------------------------+
*/

#include <Zend/zend_API.h>
#include <Zend/zend_exceptions.h>

#include "php_pht.h"
#include "src/pht_entry.h"
#include "src/classes/queue.h"

zend_object_handlers queue_handlers;
zend_class_entry *Queue_ce;

void free_queue_internal(queue_obj_internal_t *qoi)
{
    pthread_mutex_destroy(&qoi->lock);

    while (qoi->queue.size) {
        // @todo check if object is either another MQ or a HT (its refcount will
        // need to be decremented if so).
        // This should go into a specific queue_destroy method.
        pht_entry_delete(dequeue(&qoi->queue));
    }

    free(qoi);
}

static zend_object *queue_ctor(zend_class_entry *entry)
{
    queue_obj_t *qo = ecalloc(1, sizeof(queue_obj_t) + zend_object_properties_size(entry));

    zend_object_std_init(&qo->obj, entry);
    object_properties_init(&qo->obj, entry);

    qo->obj.handlers = &queue_handlers;
    qo->vn = 0;

    if (!PHT_ZG(skip_qoi_creation)) {
        queue_obj_internal_t *qoi = calloc(1, sizeof(queue_obj_internal_t));

        // qoi->state = 0;
        qoi->refcount = 1;
        qoi->vn = 0;
        pthread_mutex_init(&qoi->lock, NULL);

        qo->qoi = qoi;
    }

    return &qo->obj;
}

void qo_dtor_obj(zend_object *obj)
{
    zend_object_std_dtor(obj);
}

void qo_free_obj(zend_object *obj)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)obj - obj->handlers->offset);

    pthread_mutex_lock(&qo->qoi->lock);
    --qo->qoi->refcount;
    pthread_mutex_unlock(&qo->qoi->lock);

    if (!qo->qoi->refcount) {
        free_queue_internal(qo->qoi);
    }
}

HashTable *qo_get_properties(zval *zobj)
{
    zend_object *obj = Z_OBJ_P(zobj);
    queue_obj_t *qo = (queue_obj_t *)((char *)obj - obj->handlers->offset);

    if (obj->properties && qo->vn == qo->qoi->vn) {
        return obj->properties;
    }

    HashTable *zht = emalloc(sizeof(HashTable));

    zend_hash_init(zht, queue_size(&qo->qoi->queue), NULL, ZVAL_PTR_DTOR, 0);
    pht_queue_to_zend_hashtable(zht, &qo->qoi->queue);

    if (obj->properties) {
        // @todo safe? Perhaps just wipe HT and insert into it instead?
        GC_REFCOUNT(obj->properties) = 0;
        zend_array_destroy(obj->properties);
    }

    obj->properties = zht;
    qo->vn = qo->qoi->vn;

    return zht;
}

ZEND_BEGIN_ARG_INFO_EX(Queue_push_arginfo, 0, 0, 1)
    ZEND_ARG_INFO(0, entry)
ZEND_END_ARG_INFO()

PHP_METHOD(Queue, push)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
    zval *entry;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(entry)
    ZEND_PARSE_PARAMETERS_END();

    enqueue(&qo->qoi->queue, create_new_entry(entry));
    ++qo->qoi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Queue_pop_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Queue, pop)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pht_entry_t *entry = dequeue(&qo->qoi->queue);
    pht_convert_entry_to_zval(return_value, entry);
    pht_entry_delete(entry);
    ++qo->qoi->vn;
}

// @todo what about count() function? Rebuilding prop table is not good...
ZEND_BEGIN_ARG_INFO_EX(Queue_size_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Queue, size)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    RETVAL_LONG(queue_size(&qo->qoi->queue));
}

ZEND_BEGIN_ARG_INFO_EX(Queue_lock_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Queue, lock)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pthread_mutex_lock(&qo->qoi->lock);
}

ZEND_BEGIN_ARG_INFO_EX(Queue_unlock_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Queue, unlock)
{
    queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pthread_mutex_unlock(&qo->qoi->lock);
}

// ZEND_BEGIN_ARG_INFO_EX(Queue_set_state_arginfo, 0, 0, 1)
// ZEND_END_ARG_INFO()
//
// PHP_METHOD(Queue, setState)
// {
//     queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
//     zend_long state;
//
//     ZEND_PARSE_PARAMETERS_START(1, 1)
//         Z_PARAM_LONG(state)
//     ZEND_PARSE_PARAMETERS_END();
//
//     // @todo I don't think a mutex lock needs to be held for this?
//     // I'm going to hold it anyway for now, and performance check things later
//     pthread_mutex_lock(&qo->qoi->lock);
//     qo->qoi->state = state;
//     pthread_mutex_unlock(&qo->qoi->lock);
// }

// ZEND_BEGIN_ARG_INFO_EX(Queue_get_state_arginfo, 0, 0, 0)
// ZEND_END_ARG_INFO()
//
// PHP_METHOD(Queue, getState)
// {
//     queue_obj_t *qo = (queue_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
//
//     if (zend_parse_parameters_none() != SUCCESS) {
//         return;
//     }
//
//     // @todo I don't think a mutex lock needs to be held for this?
//     // I'm going to hold it anyway for now, and performance check things later
//     // Also, this could probably be just a simple property instead of a method
//     pthread_mutex_lock(&qo->qoi->lock);
//     RETVAL_LONG(qo->qoi->state);
//     pthread_mutex_unlock(&qo->qoi->lock);
// }

zend_function_entry Queue_methods[] = {
    PHP_ME(Queue, push, Queue_push_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Queue, pop, Queue_pop_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Queue, size, Queue_size_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Queue, lock, Queue_lock_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Queue, unlock, Queue_unlock_arginfo, ZEND_ACC_PUBLIC)
    // PHP_ME(Queue, setState, Queue_set_state_arginfo, ZEND_ACC_PUBLIC)
    // PHP_ME(Queue, getState, Queue_get_state_arginfo, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

zval *qo_read_property(zval *object, zval *member, int type, void **cache, zval *rv)
{
    zend_throw_exception(zend_ce_exception, "Properties on Queue objects are not enabled", 0);

    return &EG(uninitialized_zval);
}

void qo_write_property(zval *object, zval *member, zval *value, void **cache_slot)
{
    zend_throw_exception(zend_ce_exception, "Properties on Queue objects are not enabled", 0);
}

void queue_ce_init(void)
{
    zend_class_entry ce;
    zend_object_handlers *zh = zend_get_std_object_handlers();

    INIT_CLASS_ENTRY(ce, "Queue", Queue_methods);
    Queue_ce = zend_register_internal_class(&ce);
    Queue_ce->create_object = queue_ctor;

    memcpy(&queue_handlers, zh, sizeof(zend_object_handlers));

    queue_handlers.offset = XtOffsetOf(queue_obj_t, obj);
    queue_handlers.dtor_obj = qo_dtor_obj;
    queue_handlers.free_obj = qo_free_obj;
    queue_handlers.read_property = qo_read_property;
    queue_handlers.write_property = qo_write_property;
    queue_handlers.get_properties = qo_get_properties;
}