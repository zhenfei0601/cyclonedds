/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>
#include <string.h>
#include "dds/dds.h"
#include "dds/version.h"
#include "dds__subscriber.h"
#include "dds__reader.h"
#include "dds__listener.h"
#include "dds__qos.h"
#include "dds__init.h"
#include "dds__rhc.h"
#include "dds__err.h"
#include "dds__topic.h"
#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_thread.h"
#include "dds/ddsi/q_globals.h"
#include "dds__builtin.h"
#include "dds/ddsi/ddsi_sertopic.h"

DECL_ENTITY_LOCK_UNLOCK(extern inline, dds_reader)

#define DDS_READER_STATUS_MASK                                   \
                        DDS_SAMPLE_REJECTED_STATUS              |\
                        DDS_LIVELINESS_CHANGED_STATUS           |\
                        DDS_REQUESTED_DEADLINE_MISSED_STATUS    |\
                        DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS   |\
                        DDS_DATA_AVAILABLE_STATUS               |\
                        DDS_SAMPLE_LOST_STATUS                  |\
                        DDS_SUBSCRIPTION_MATCHED_STATUS

static dds_return_t
dds_reader_instance_hdl(
        dds_entity *e,
        dds_instance_handle_t *i)
{
    assert(e);
    assert(i);
    *i = (dds_instance_handle_t)reader_instance_id(&e->m_guid);
    return DDS_RETCODE_OK;
}

static dds_return_t
dds_reader_close(
        dds_entity *e)
{
    dds_retcode_t rc;
    dds_return_t ret = DDS_RETCODE_OK;

    assert(e);

    thread_state_awake (lookup_thread_state ());
    if (delete_reader(&e->m_guid) != 0) {
        DDS_ERROR("Internal error");
        rc = DDS_RETCODE_ERROR;
        ret = DDS_ERRNO(rc);
    }
    thread_state_asleep (lookup_thread_state ());
    return ret;
}

static dds_return_t
dds_reader_delete(
        dds_entity *e)
{
    dds_reader *rd = (dds_reader*)e;
    dds_return_t ret;
    assert(e);
    ret = dds_delete(rd->m_topic->m_entity.m_hdllink.hdl);
    if(ret == DDS_RETCODE_OK){
        ret = dds_delete_impl(e->m_parent->m_hdllink.hdl, true);
        if(dds_err_nr(ret) == DDS_RETCODE_BAD_PARAMETER){
            ret = DDS_RETCODE_OK;
        }
    }
    dds_free(rd->m_loan);
    return ret;
}

static dds_return_t
dds_reader_qos_validate(
        const dds_qos_t *qos,
        bool enabled)
{
    dds_return_t ret = DDS_RETCODE_OK;

    assert(qos);

    /* Check consistency. */
    if(!dds_qos_validate_common(qos)) {
        DDS_ERROR("Argument Qos is not valid\n");
        ret = DDS_ERRNO(DDS_RETCODE_ERROR);
    }
    if((qos->present & QP_USER_DATA) && !(validate_octetseq (&qos->user_data))) {
        DDS_ERROR("User data policy is inconsistent and caused an error\n");
        ret = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
    }
    if((qos->present & QP_PRISMTECH_READER_DATA_LIFECYCLE) && (validate_reader_data_lifecycle (&qos->reader_data_lifecycle) != 0)) {
        DDS_ERROR("Prismtech reader data lifecycle policy is inconsistent and caused an error\n");
        ret = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
    }
    if((qos->present & QP_TIME_BASED_FILTER) && (validate_duration (&qos->time_based_filter.minimum_separation) != 0)) {
        DDS_ERROR("Time based filter policy is inconsistent and caused an error\n");
        ret = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
    }
    if((qos->present & QP_HISTORY) && (qos->present & QP_RESOURCE_LIMITS) && (validate_history_and_resource_limits (&qos->history, &qos->resource_limits) != 0)) {
        DDS_ERROR("History and resource limits policy is inconsistent and caused an error\n");
        ret = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
    }
    if((qos->present & QP_TIME_BASED_FILTER) && (qos->present & QP_DEADLINE) && !(validate_deadline_and_timebased_filter (qos->deadline.deadline, qos->time_based_filter.minimum_separation))) {
        DDS_ERROR("Time based filter and deadline policy is inconsistent and caused an error\n");
        ret = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
    }
    if(ret == DDS_RETCODE_OK && enabled) {
        ret = dds_qos_validate_mutable_common(qos);
    }

    return ret;
}

static dds_return_t
dds_reader_qos_set(
        dds_entity *e,
        const dds_qos_t *qos,
        bool enabled)
{
    dds_return_t ret = dds_reader_qos_validate(qos, enabled);
    (void)e;
    if (ret == DDS_RETCODE_OK) {
        if (enabled) {
            /* TODO: CHAM-95: DDSI does not support changing QoS policies. */
            DDS_ERROR(DDS_PROJECT_NAME" does not support changing QoS policies\n");
            ret = DDS_ERRNO(DDS_RETCODE_UNSUPPORTED);
        }
    }
    return ret;
}

static dds_return_t
dds_reader_status_validate(
        uint32_t mask)
{
    return (mask & ~(DDS_READER_STATUS_MASK)) ?
                     DDS_ERRNO(DDS_RETCODE_BAD_PARAMETER) :
                     DDS_RETCODE_OK;
}

void dds_reader_data_available_cb (struct dds_reader *rd)
{
  /* DATA_AVAILABLE is special in two ways: firstly, it should first try
     DATA_ON_READERS on the line of ancestors, and if not consumed set the
     status on the subscriber; secondly it is the only one for which
     overhead really matters.  Otherwise, it is pretty much like
     dds_reader_status_cb. */

  ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
  if (!(rd->m_entity.m_status_enable & DDS_DATA_AVAILABLE_STATUS))
  {
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    return;
  }

  while (rd->m_entity.m_cb_count > 0)
    ddsrt_cond_wait (&rd->m_entity.m_observers_cond, &rd->m_entity.m_observers_lock);
  rd->m_entity.m_cb_count++;

  struct dds_listener const * const lst = &rd->m_entity.m_listener;
  dds_entity * const sub = rd->m_entity.m_parent;
  if (lst->on_data_on_readers)
  {
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);

    ddsrt_mutex_lock (&sub->m_observers_lock);
    while (sub->m_cb_count > 0)
      ddsrt_cond_wait (&sub->m_observers_cond, &sub->m_observers_lock);
    sub->m_cb_count++;
    ddsrt_mutex_unlock (&sub->m_observers_lock);

    lst->on_data_on_readers (sub->m_hdllink.hdl, lst->on_data_on_readers_arg);

    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    ddsrt_mutex_lock (&sub->m_observers_lock);
    sub->m_cb_count--;
    ddsrt_cond_broadcast (&sub->m_observers_cond);
    ddsrt_mutex_unlock (&sub->m_observers_lock);
  }
  else if (rd->m_entity.m_listener.on_data_available)
  {
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    lst->on_data_available (rd->m_entity.m_hdllink.hdl, lst->on_data_available_arg);
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
  }
  else
  {
    dds_entity_status_set (&rd->m_entity, DDS_DATA_AVAILABLE_STATUS);

    ddsrt_mutex_lock (&sub->m_observers_lock);
    dds_entity_status_set (sub, DDS_DATA_ON_READERS_STATUS);
    ddsrt_mutex_unlock (&sub->m_observers_lock);
  }

  rd->m_entity.m_cb_count--;
  ddsrt_cond_broadcast (&rd->m_entity.m_observers_cond);
  ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
}

void dds_reader_status_cb (void *ventity, const status_cb_data_t *data)
{
  struct dds_entity * const entity = ventity;

  /* When data is NULL, it means that the DDSI reader is deleted. */
  if (data == NULL)
  {
    /* Release the initial claim that was done during the create. This
     * will indicate that further API deletion is now possible. */
    dds_handle_release (&entity->m_hdllink);
    return;
  }

  struct dds_listener const * const lst = &entity->m_listener;
  enum dds_status_id status_id = (enum dds_status_id) data->raw_status_id;
  bool invoke = false;
  void *vst = NULL;
  int32_t *reset[2] = { NULL, NULL };

  /* DATA_AVAILABLE is handled by dds_reader_data_available_cb */
  assert (status_id != DDS_DATA_AVAILABLE_STATUS_ID);

  /* Serialize listener invocations -- it is somewhat sad to do this,
     but then it may also be unreasonable to expect the application to
     handle concurrent invocations of a single listener.  The benefit
     here is that it means the counters and "change" counters
     can safely be incremented and/or reset while releasing
     m_observers_lock for the duration of the listener call itself,
     and that similarly the listener function and argument pointers
     are stable */
  ddsrt_mutex_lock (&entity->m_observers_lock);
  while (entity->m_cb_count > 0)
    ddsrt_cond_wait (&entity->m_observers_cond, &entity->m_observers_lock);
  entity->m_cb_count++;

  /* Update status metrics. */
  dds_reader * const rd = (dds_reader *) entity;
  switch (status_id) {
    case DDS_REQUESTED_DEADLINE_MISSED_STATUS_ID: {
      struct dds_requested_deadline_missed_status * const st = vst = &rd->m_requested_deadline_missed_status;
      st->last_instance_handle = data->handle;
      st->total_count++;
      st->total_count_change++;
      invoke = (lst->on_requested_deadline_missed != 0);
      reset[0] = &st->total_count_change;
      break;
    }
    case DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS_ID: {
      struct dds_requested_incompatible_qos_status * const st = vst = &rd->m_requested_incompatible_qos_status;
      st->total_count++;
      st->total_count_change++;
      st->last_policy_id = data->extra;
      invoke = (lst->on_requested_incompatible_qos != 0);
      reset[0] = &st->total_count_change;
      break;
    }
    case DDS_SAMPLE_LOST_STATUS_ID: {
      struct dds_sample_lost_status * const st = vst = &rd->m_sample_lost_status;
      st->total_count++;
      st->total_count_change++;
      invoke = (lst->on_sample_lost != 0);
      reset[0] = &st->total_count_change;
      break;
    }
    case DDS_SAMPLE_REJECTED_STATUS_ID: {
      struct dds_sample_rejected_status * const st = vst = &rd->m_sample_rejected_status;
      st->total_count++;
      st->total_count_change++;
      st->last_reason = data->extra;
      st->last_instance_handle = data->handle;
      invoke = (lst->on_sample_rejected != 0);
      reset[0] = &st->total_count_change;
      break;
    }
    case DDS_LIVELINESS_CHANGED_STATUS_ID: {
      struct dds_liveliness_changed_status * const st = vst = &rd->m_liveliness_changed_status;
      if (data->add) {
        st->alive_count++;
        st->alive_count_change++;
        if (st->not_alive_count > 0) {
          st->not_alive_count--;
        }
      } else {
        st->alive_count--;
        st->not_alive_count++;
        st->not_alive_count_change++;
      }
      st->last_publication_handle = data->handle;
      invoke = (lst->on_liveliness_changed != 0);
      reset[0] = &st->alive_count_change;
      reset[1] = &st->not_alive_count_change;
      break;
    }
    case DDS_SUBSCRIPTION_MATCHED_STATUS_ID: {
      struct dds_subscription_matched_status * const st = vst = &rd->m_subscription_matched_status;
      if (data->add) {
        st->total_count++;
        st->total_count_change++;
        st->current_count++;
        st->current_count_change++;
      } else {
        st->current_count--;
        st->current_count_change--;
      }
      st->last_publication_handle = data->handle;
      invoke = (lst->on_subscription_matched != 0);
      reset[0] = &st->total_count_change;
      reset[1] = &st->current_count_change;
      break;
    }
    case DDS_DATA_ON_READERS_STATUS_ID:
    case DDS_DATA_AVAILABLE_STATUS_ID:
    case DDS_INCONSISTENT_TOPIC_STATUS_ID:
    case DDS_LIVELINESS_LOST_STATUS_ID:
    case DDS_PUBLICATION_MATCHED_STATUS_ID:
    case DDS_OFFERED_DEADLINE_MISSED_STATUS_ID:
    case DDS_OFFERED_INCOMPATIBLE_QOS_STATUS_ID:
      assert (0);
  }

  if (invoke)
  {
    ddsrt_mutex_unlock (&entity->m_observers_lock);
    dds_entity_invoke_listener(entity, status_id, vst);
    ddsrt_mutex_lock (&entity->m_observers_lock);
    *reset[0] = 0;
    if (reset[1])
      *reset[1] = 0;
  }
  else
  {
    dds_entity_status_set (entity, 1u << status_id);
  }

  entity->m_cb_count--;
  ddsrt_cond_broadcast (&entity->m_observers_cond);
  ddsrt_mutex_unlock (&entity->m_observers_lock);
}

dds_entity_t
dds_create_reader(
    dds_entity_t participant_or_subscriber,
    dds_entity_t topic,
    const dds_qos_t *qos,
    const dds_listener_t *listener)
{
    dds_qos_t * rqos;
    dds_retcode_t rc;
    dds_subscriber * sub = NULL;
    dds_entity_t subscriber;
    dds_reader * rd;
    struct rhc * rhc;
    dds_topic * tp;
    dds_entity_t reader;
    dds_entity_t t;
    dds_return_t ret = DDS_RETCODE_OK;
    bool internal_topic;

    switch (topic) {
        case DDS_BUILTIN_TOPIC_DCPSPARTICIPANT:
        case DDS_BUILTIN_TOPIC_DCPSTOPIC:
        case DDS_BUILTIN_TOPIC_DCPSPUBLICATION:
        case DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION:
            internal_topic = true;
            subscriber = dds__get_builtin_subscriber(participant_or_subscriber);
            t = dds__get_builtin_topic (subscriber, topic);
            break;

        default: {
            dds_entity *p_or_s;
            if ((rc = dds_entity_claim (participant_or_subscriber, &p_or_s)) != DDS_RETCODE_OK) {
                return DDS_ERRNO (rc);
            }
            if (dds_entity_kind (p_or_s) == DDS_KIND_PARTICIPANT) {
                subscriber = dds_create_subscriber(participant_or_subscriber, qos, NULL);
            } else {
                subscriber = participant_or_subscriber;
            }
            dds_entity_release (p_or_s);
            internal_topic = false;
            t = topic;
            break;
        }
    }

    if ((rc = dds_subscriber_lock (subscriber, &sub)) != DDS_RETCODE_OK) {
        reader = DDS_ERRNO (rc);
        goto err_sub_lock;
    }

    if ((subscriber != participant_or_subscriber) && !internal_topic) {
        /* Delete implicit subscriber if reader creation fails */
        sub->m_entity.m_flags |= DDS_ENTITY_IMPLICIT;
    }

    rc = dds_topic_lock(t, &tp);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking topic\n");
        reader = DDS_ERRNO(rc);
        goto err_tp_lock;
    }
    assert (tp->m_stopic);
    assert (sub->m_entity.m_domain == tp->m_entity.m_domain);

    /* Merge qos from topic and subscriber */
    rqos = dds_create_qos ();
    if (qos) {
        /* Only returns failure when one of the qos args is NULL, which
         * is not the case here. */
        (void)dds_copy_qos(rqos, qos);
    }

    if(sub->m_entity.m_qos){
        dds_merge_qos (rqos, sub->m_entity.m_qos);
    }

    if (tp->m_entity.m_qos) {
        dds_merge_qos (rqos, tp->m_entity.m_qos);

        /* reset the following qos policies if set during topic qos merge as they aren't applicable for reader */
        rqos->present &= ~(QP_DURABILITY_SERVICE | QP_TRANSPORT_PRIORITY | QP_LIFESPAN);
    }
    nn_xqos_mergein_missing (rqos, &gv.default_xqos_rd);

    ret = dds_reader_qos_validate (rqos, false);
    if (ret != 0) {
        dds_delete_qos(rqos);
        reader = ret;
        goto err_bad_qos;
    }

    /* Additional checks required for built-in topics */
    if (internal_topic && !dds__validate_builtin_reader_qos(topic, qos)) {
        dds_delete_qos(rqos);
        DDS_ERROR("Invalid QoS specified for built-in topic reader");
        reader = DDS_ERRNO(DDS_RETCODE_INCONSISTENT_POLICY);
        goto err_bad_qos;
    }

    /* Create reader and associated read cache */
    rd = dds_alloc (sizeof (*rd));
    reader = dds_entity_init (&rd->m_entity, &sub->m_entity, DDS_KIND_READER, rqos, listener, DDS_READER_STATUS_MASK);
    rd->m_sample_rejected_status.last_reason = DDS_NOT_REJECTED;
    rd->m_topic = tp;
    rhc = dds_rhc_new (rd, tp->m_stopic);
    dds_entity_add_ref_nolock (&tp->m_entity);
    rd->m_entity.m_deriver.close = dds_reader_close;
    rd->m_entity.m_deriver.delete = dds_reader_delete;
    rd->m_entity.m_deriver.set_qos = dds_reader_qos_set;
    rd->m_entity.m_deriver.validate_status = dds_reader_status_validate;
    rd->m_entity.m_deriver.get_instance_hdl = dds_reader_instance_hdl;

    /* Extra claim of this reader to make sure that the delete waits until DDSI
     * has deleted its reader as well. This can be known through the callback. */
    dds_handle_claim_inc (&rd->m_entity.m_hdllink);

    ddsrt_mutex_unlock(&tp->m_entity.m_mutex);
    ddsrt_mutex_unlock(&sub->m_entity.m_mutex);

    thread_state_awake (lookup_thread_state ());
    ret = new_reader(&rd->m_rd, &rd->m_entity.m_guid, NULL, &sub->m_entity.m_participant->m_guid, tp->m_stopic,
                     rqos, rhc, dds_reader_status_cb, rd);
    ddsrt_mutex_lock(&sub->m_entity.m_mutex);
    ddsrt_mutex_lock(&tp->m_entity.m_mutex);
    assert (ret == DDS_RETCODE_OK);
    thread_state_asleep (lookup_thread_state ());

    /* For persistent data register reader with durability */
    if (dds_global.m_dur_reader && (rd->m_entity.m_qos->durability.kind > NN_TRANSIENT_LOCAL_DURABILITY_QOS)) {
        (dds_global.m_dur_reader) (rd, rhc);
    }
    dds_topic_unlock(tp);
    dds_subscriber_unlock(sub);

    if (internal_topic) {
        /* If topic is builtin, then the topic entity is local and should
         * be deleted because the application won't. */
        dds_delete(t);
    }

    return reader;

err_bad_qos:
    dds_topic_unlock(tp);
err_tp_lock:
    dds_subscriber_unlock(sub);
    if((sub->m_entity.m_flags & DDS_ENTITY_IMPLICIT) != 0){
        (void)dds_delete(subscriber);
    }
err_sub_lock:
    if (internal_topic) {
        /* If topic is builtin, then the topic entity is local and should
         * be deleted because the application won't. */
        dds_delete(t);
    }
    return reader;
}

void dds_reader_ddsi2direct (dds_entity_t entity, ddsi2direct_directread_cb_t cb, void *cbarg)
{
  dds_entity *dds_entity;
  if (dds_entity_claim(entity, &dds_entity) != DDS_RETCODE_OK)
    return;

  if (dds_entity_kind (dds_entity) != DDS_KIND_READER)
  {
    dds_entity_release (dds_entity);
    return;
  }

  dds_reader *dds_rd = (dds_reader *) dds_entity;
  struct reader *rd = dds_rd->m_rd;
  nn_guid_t pwrguid;
  struct proxy_writer *pwr;
  struct rd_pwr_match *m;
  memset (&pwrguid, 0, sizeof (pwrguid));
  ddsrt_mutex_lock (&rd->e.lock);

  rd->ddsi2direct_cb = cb;
  rd->ddsi2direct_cbarg = cbarg;
  while ((m = ddsrt_avl_lookup_succ_eq (&rd_writers_treedef, &rd->writers, &pwrguid)) != NULL)
  {
    /* have to be careful walking the tree -- pretty is different, but
     I want to check this before I write a lookup_succ function. */
    struct rd_pwr_match *m_next;
    nn_guid_t pwrguid_next;
    pwrguid = m->pwr_guid;
    if ((m_next = ddsrt_avl_find_succ (&rd_writers_treedef, &rd->writers, m)) != NULL)
      pwrguid_next = m_next->pwr_guid;
    else
    {
      memset (&pwrguid_next, 0xff, sizeof (pwrguid_next));
      pwrguid_next.entityid.u = (pwrguid_next.entityid.u & ~(uint32_t)0xff) | NN_ENTITYID_KIND_WRITER_NO_KEY;
    }
    ddsrt_mutex_unlock (&rd->e.lock);
    if ((pwr = ephash_lookup_proxy_writer_guid (&pwrguid)) != NULL)
    {
      ddsrt_mutex_lock (&pwr->e.lock);
      pwr->ddsi2direct_cb = cb;
      pwr->ddsi2direct_cbarg = cbarg;
      ddsrt_mutex_unlock (&pwr->e.lock);
    }
    pwrguid = pwrguid_next;
    ddsrt_mutex_lock (&rd->e.lock);
  }
  ddsrt_mutex_unlock (&rd->e.lock);
  dds_entity_release (dds_entity);
}

uint32_t dds_reader_lock_samples (dds_entity_t reader)
{
  dds_reader *rd;
  uint32_t n;
  if (dds_reader_lock (reader, &rd) != DDS_RETCODE_OK)
    return 0;
  n = dds_rhc_lock_samples (rd->m_rd->rhc);
  dds_reader_unlock (rd);
  return n;
}

int dds_reader_wait_for_historical_data (dds_entity_t reader, dds_duration_t max_wait)
{
  dds_reader *rd;
  int ret;
  if ((ret = dds_reader_lock (reader, &rd)) != DDS_RETCODE_OK)
    return DDS_ERRNO (ret);
  switch (rd->m_entity.m_qos->durability.kind)
  {
    case DDS_DURABILITY_VOLATILE:
      ret = DDS_RETCODE_OK;
      break;
    case DDS_DURABILITY_TRANSIENT_LOCAL:
      break;
    case DDS_DURABILITY_TRANSIENT:
    case DDS_DURABILITY_PERSISTENT:
      ret = (dds_global.m_dur_wait) (rd, max_wait);
      break;
  }
  dds_reader_unlock(rd);
  return ret;
}

dds_entity_t dds_get_subscriber (dds_entity_t entity)
{
  dds_entity *e;
  dds_retcode_t ret;
  if ((ret = dds_entity_claim (entity, &e)) != DDS_RETCODE_OK)
    return (dds_entity_t) DDS_ERRNO (ret);
  else
  {
    dds_entity_t subh;
    switch (dds_entity_kind (e))
    {
      case DDS_KIND_READER:
        assert (dds_entity_kind (e->m_parent) == DDS_KIND_SUBSCRIBER);
        subh = e->m_parent->m_hdllink.hdl;
        break;
      case DDS_KIND_COND_READ:
      case DDS_KIND_COND_QUERY:
        assert (dds_entity_kind (e->m_parent) == DDS_KIND_READER);
        assert (dds_entity_kind (e->m_parent->m_parent) == DDS_KIND_SUBSCRIBER);
        subh = e->m_parent->m_parent->m_hdllink.hdl;
        break;
      default:
        subh = DDS_ERRNO (DDS_RETCODE_ILLEGAL_OPERATION);
        break;
    }
    dds_entity_release (e);
    return subh;
  }
}

dds_return_t
dds_get_subscription_matched_status (
    dds_entity_t reader,
    dds_subscription_matched_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_subscription_matched_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_SUBSCRIPTION_MATCHED_STATUS) {
        rd->m_subscription_matched_status.total_count_change = 0;
        rd->m_subscription_matched_status.current_count_change = 0;
        dds_entity_status_reset(&rd->m_entity, DDS_SUBSCRIPTION_MATCHED_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}

dds_return_t
dds_get_liveliness_changed_status (
    dds_entity_t reader,
    dds_liveliness_changed_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_liveliness_changed_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_LIVELINESS_CHANGED_STATUS) {
        rd->m_liveliness_changed_status.alive_count_change = 0;
        rd->m_liveliness_changed_status.not_alive_count_change = 0;
        dds_entity_status_reset(&rd->m_entity, DDS_LIVELINESS_CHANGED_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}

dds_return_t dds_get_sample_rejected_status (
    dds_entity_t reader,
    dds_sample_rejected_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_sample_rejected_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_SAMPLE_REJECTED_STATUS) {
        rd->m_sample_rejected_status.total_count_change = 0;
        rd->m_sample_rejected_status.last_reason = DDS_NOT_REJECTED;
        dds_entity_status_reset(&rd->m_entity, DDS_SAMPLE_REJECTED_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}

dds_return_t dds_get_sample_lost_status (
    dds_entity_t reader,
    dds_sample_lost_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_sample_lost_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_SAMPLE_LOST_STATUS) {
        rd->m_sample_lost_status.total_count_change = 0;
        dds_entity_status_reset(&rd->m_entity, DDS_SAMPLE_LOST_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}

dds_return_t dds_get_requested_deadline_missed_status (
    dds_entity_t reader,
    dds_requested_deadline_missed_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_requested_deadline_missed_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_REQUESTED_DEADLINE_MISSED_STATUS) {
        rd->m_requested_deadline_missed_status.total_count_change = 0;
        dds_entity_status_reset(&rd->m_entity, DDS_REQUESTED_DEADLINE_MISSED_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}

dds_return_t dds_get_requested_incompatible_qos_status (
    dds_entity_t reader,
    dds_requested_incompatible_qos_status_t * status)
{
    dds_retcode_t rc;
    dds_reader *rd;
    dds_return_t ret = DDS_RETCODE_OK;

    rc = dds_reader_lock(reader, &rd);
    if (rc != DDS_RETCODE_OK) {
        DDS_ERROR("Error occurred on locking reader\n");
        ret = DDS_ERRNO(rc);
        goto fail;
    }
    /* status = NULL, application do not need the status, but reset the counter & triggered bit */
    if (status) {
        *status = rd->m_requested_incompatible_qos_status;
    }
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    if (rd->m_entity.m_status_enable & DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS) {
        rd->m_requested_incompatible_qos_status.total_count_change = 0;
        dds_entity_status_reset(&rd->m_entity, DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS);
    }
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);
    dds_reader_unlock(rd);
fail:
    return ret;
}
